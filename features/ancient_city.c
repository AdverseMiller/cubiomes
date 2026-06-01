#include "ancient_city.h"

#include <stdlib.h>
#include <string.h>

#define AC_MAX_PLACED 256
#define AC_MAX_SHAPES 256
#define AC_MAX_HOLES 256
#define AC_MAX_CHUNKS 256

enum AcDirection {
    AC_DOWN,
    AC_UP,
    AC_NORTH,
    AC_SOUTH,
    AC_WEST,
    AC_EAST,
};

enum AcElementType {
    AC_ELEMENT_EMPTY,
    AC_ELEMENT_SINGLE,
    AC_ELEMENT_LIST,
    AC_ELEMENT_FEATURE,
};

typedef struct {
    int8_t x, y, z;
    uint8_t front, top;
    int16_t name, target, pool;
} AcJigsaw;

typedef struct {
    int8_t x, y, z;
    const char *lootTable;
} AcChest;

typedef struct {
    const char *name;
    int8_t sx, sy, sz;
    int16_t jigsawFirst, jigsawCount;
    int16_t chestFirst, chestCount;
} AcTemplate;

typedef struct {
    uint8_t type;
    const char *name;
    int16_t templateId;
    int16_t firstChild, childCount;
} AcElement;

typedef struct {
    int16_t first, count;
} AcPool;

#include "ancient_city_data.inc"

typedef struct {
    int x, y, z;
} AcPos;

typedef struct {
    int minX, minY, minZ;
    int maxX, maxY, maxZ;
} AcBox;

typedef struct {
    double minX, minY, minZ;
    double maxX, maxY, maxZ;
} AcAabb;

typedef struct {
    AcAabb allowed;
    AcAabb holes[AC_MAX_HOLES];
    int holeCount;
} AcFreeShape;

typedef struct {
    AcPos pos;
    uint8_t front, top;
    int16_t name, target, pool;
} AcPlacedJigsaw;

typedef struct {
    int elementId;
    AcPos pos;
    AcBox box;
    uint8_t rot;
    int8_t depth;
    int freeShape;
} AcPlacedPiece;

typedef struct {
    int cx, cz, count;
} AcChunkLootState;

static const int ac_dir_x[] = {0, 0, 0, 0, -1, 1};
static const int ac_dir_y[] = {-1, 1, 0, 0, 0, 0};
static const int ac_dir_z[] = {0, 0, -1, 1, 0, 0};
static const uint8_t ac_opp[] = {AC_UP, AC_DOWN, AC_SOUTH, AC_NORTH, AC_EAST, AC_WEST};

static inline int ac_min(int a, int b) { return a < b ? a : b; }
static inline int ac_max(int a, int b) { return a > b ? a : b; }

static uint8_t ac_rotate_dir(uint8_t dir, uint8_t rot)
{
    if (dir == AC_DOWN || dir == AC_UP)
        return dir;
    static const uint8_t horizontal[4] = {AC_NORTH, AC_EAST, AC_SOUTH, AC_WEST};
    int idx;
    switch (dir) {
    case AC_NORTH: idx = 0; break;
    case AC_EAST: idx = 1; break;
    case AC_SOUTH: idx = 2; break;
    case AC_WEST: idx = 3; break;
    default: idx = 0; break;
    }
    return horizontal[(idx + rot) & 3];
}

static AcPos ac_transform(AcPos pos, uint8_t rot)
{
    switch (rot) {
    case 1: return (AcPos) {-pos.z, pos.y,  pos.x};
    case 2: return (AcPos) {-pos.x, pos.y, -pos.z};
    case 3: return (AcPos) { pos.z, pos.y, -pos.x};
    default: return pos;
    }
}

static AcPos ac_add(AcPos a, AcPos b)
{
    return (AcPos) {a.x + b.x, a.y + b.y, a.z + b.z};
}

static AcPos ac_sub(AcPos a, AcPos b)
{
    return (AcPos) {a.x - b.x, a.y - b.y, a.z - b.z};
}

static AcBox ac_box_from_corners(AcPos a, AcPos b)
{
    return (AcBox) {
        ac_min(a.x, b.x), ac_min(a.y, b.y), ac_min(a.z, b.z),
        ac_max(a.x, b.x), ac_max(a.y, b.y), ac_max(a.z, b.z),
    };
}

static AcBox ac_move_box(AcBox box, int dx, int dy, int dz)
{
    box.minX += dx; box.maxX += dx;
    box.minY += dy; box.maxY += dy;
    box.minZ += dz; box.maxZ += dz;
    return box;
}

static AcBox ac_template_box(int templateId, AcPos pos, uint8_t rot)
{
    const AcTemplate *template = &ac_templates[templateId];
    AcPos corner0 = ac_transform((AcPos) {0, 0, 0}, rot);
    AcPos corner1 = ac_transform((AcPos) {template->sx - 1, template->sy - 1, template->sz - 1}, rot);
    AcBox box = ac_box_from_corners(corner0, corner1);
    return ac_move_box(box, pos.x, pos.y, pos.z);
}

static AcBox ac_union_box(AcBox a, AcBox b)
{
    return (AcBox) {
        ac_min(a.minX, b.minX), ac_min(a.minY, b.minY), ac_min(a.minZ, b.minZ),
        ac_max(a.maxX, b.maxX), ac_max(a.maxY, b.maxY), ac_max(a.maxZ, b.maxZ),
    };
}

static AcBox ac_element_box(int elementId, AcPos pos, uint8_t rot)
{
    const AcElement *element = &ac_elements[elementId];
    switch (element->type) {
    case AC_ELEMENT_SINGLE:
        return ac_template_box(element->templateId, pos, rot);
    case AC_ELEMENT_LIST: {
        AcBox box = ac_template_box(ac_list_children[element->firstChild], pos, rot);
        for (int i = 1; i < element->childCount; i++)
            box = ac_union_box(box, ac_template_box(ac_list_children[element->firstChild + i], pos, rot));
        return box;
    }
    case AC_ELEMENT_FEATURE:
        return (AcBox) {pos.x, pos.y, pos.z, pos.x, pos.y, pos.z};
    default:
        return (AcBox) {pos.x, pos.y, pos.z, pos.x, pos.y, pos.z};
    }
}

static int ac_box_contains_pos(AcBox box, AcPos pos)
{
    return pos.x >= box.minX && pos.x <= box.maxX &&
           pos.y >= box.minY && pos.y <= box.maxY &&
           pos.z >= box.minZ && pos.z <= box.maxZ;
}

static AcAabb ac_aabb_from_box(AcBox box)
{
    return (AcAabb) {
        box.minX, box.minY, box.minZ,
        box.maxX + 1.0, box.maxY + 1.0, box.maxZ + 1.0,
    };
}

static AcAabb ac_aabb_deflate(AcAabb aabb, double amount)
{
    return (AcAabb) {
        aabb.minX + amount, aabb.minY + amount, aabb.minZ + amount,
        aabb.maxX - amount, aabb.maxY - amount, aabb.maxZ - amount,
    };
}

static int ac_aabb_contains(AcAabb a, AcAabb b)
{
    return b.minX >= a.minX && b.maxX <= a.maxX &&
           b.minY >= a.minY && b.maxY <= a.maxY &&
           b.minZ >= a.minZ && b.maxZ <= a.maxZ;
}

static int ac_aabb_intersects(AcAabb a, AcAabb b)
{
    return a.minX < b.maxX && a.maxX > b.minX &&
           a.minY < b.maxY && a.maxY > b.minY &&
           a.minZ < b.maxZ && a.maxZ > b.minZ;
}

static int ac_free_can_place(const AcFreeShape *shape, AcBox box)
{
    AcAabb target = ac_aabb_deflate(ac_aabb_from_box(box), 0.25);
    if (!ac_aabb_contains(shape->allowed, target))
        return 0;
    for (int i = 0; i < shape->holeCount; i++) {
        if (ac_aabb_intersects(shape->holes[i], target))
            return 0;
    }
    return 1;
}

static int ac_free_carve(AcFreeShape *shape, AcBox box)
{
    if (shape->holeCount >= AC_MAX_HOLES)
        return 0;
    shape->holes[shape->holeCount++] = ac_aabb_from_box(box);
    return 1;
}

static void ac_shuffle_ints(int *values, int count, uint64_t *rng)
{
    for (int i = count; i > 1; i--) {
        int swapTo = nextInt(rng, i);
        int tmp = values[i - 1];
        values[i - 1] = values[swapTo];
        values[swapTo] = tmp;
    }
}

static int ac_get_jigsaws(int elementId, AcPos pos, uint8_t rot, uint64_t *rng, AcPlacedJigsaw *out)
{
    const AcElement *element = &ac_elements[elementId];
    int templateId = -1;
    if (element->type == AC_ELEMENT_SINGLE) {
        templateId = element->templateId;
    } else if (element->type == AC_ELEMENT_LIST) {
        templateId = ac_list_children[element->firstChild];
    } else if (element->type == AC_ELEMENT_FEATURE) {
        out[0] = (AcPlacedJigsaw) {
            pos, AC_DOWN, AC_SOUTH,
            AC_ID_MINECRAFT_BOTTOM,
            AC_ID_MINECRAFT_EMPTY,
            AC_POOL_MINECRAFT_EMPTY,
        };
        return 1;
    } else {
        return 0;
    }

    const AcTemplate *template = &ac_templates[templateId];
    for (int i = 0; i < template->jigsawCount; i++) {
        const AcJigsaw *jigsaw = &ac_jigsaws[template->jigsawFirst + i];
        AcPos local = ac_transform((AcPos) {jigsaw->x, jigsaw->y, jigsaw->z}, rot);
        out[i] = (AcPlacedJigsaw) {
            ac_add(local, pos),
            ac_rotate_dir(jigsaw->front, rot),
            ac_rotate_dir(jigsaw->top, rot),
            jigsaw->name,
            jigsaw->target,
            jigsaw->pool,
        };
    }

    for (int i = template->jigsawCount; i > 1; i--) {
        int swapTo = nextInt(rng, i);
        AcPlacedJigsaw tmp = out[i - 1];
        out[i - 1] = out[swapTo];
        out[swapTo] = tmp;
    }
    return template->jigsawCount;
}

static int ac_can_attach(const AcPlacedJigsaw *source, const AcPlacedJigsaw *target)
{
    return source->front == ac_opp[target->front] && source->target == target->name;
}

static int ac_random_pool_element(int poolId, uint64_t *rng)
{
    const AcPool *pool = &ac_pools[poolId];
    if (pool->count <= 0)
        return -1;
    return ac_pool_entries[pool->first + nextInt(rng, pool->count)];
}

static int ac_shuffled_pool(int poolId, uint64_t *rng, int *out)
{
    const AcPool *pool = &ac_pools[poolId];
    for (int i = 0; i < pool->count; i++)
        out[i] = ac_pool_entries[pool->first + i];
    ac_shuffle_ints(out, pool->count, rng);
    return pool->count;
}

static int ac_add_shape(AcFreeShape *shapes, int *shapeCount, AcAabb allowed)
{
    if (*shapeCount >= AC_MAX_SHAPES)
        return -1;
    int idx = (*shapeCount)++;
    shapes[idx].allowed = allowed;
    shapes[idx].holeCount = 0;
    return idx;
}

static int ac_add_piece(AcPlacedPiece *pieces, int *pieceCount, int elementId, AcPos pos, uint8_t rot, int8_t depth, int freeShape)
{
    if (*pieceCount >= AC_MAX_PLACED)
        return -1;
    int idx = (*pieceCount)++;
    pieces[idx].elementId = elementId;
    pieces[idx].pos = pos;
    pieces[idx].rot = rot;
    pieces[idx].depth = depth;
    pieces[idx].box = ac_element_box(elementId, pos, rot);
    pieces[idx].freeShape = freeShape;
    return idx;
}

static int ac_generate_pieces(AcPlacedPiece *pieces, AcFreeShape *shapes, int *pieceCount, int *shapeCount, uint64_t seed, int chunkX, int chunkZ)
{
    uint64_t rng = chunkGenerateRnd(seed, chunkX, chunkZ);
    uint8_t centerRot = nextInt(&rng, 4);
    int centerElement = ac_random_pool_element(AC_POOL_MINECRAFT_ANCIENT_CITY_CITY_CENTER, &rng);
    if (centerElement < 0)
        return 0;

    AcPos startPos = {chunkX << 4, -27, chunkZ << 4};
    AcPlacedJigsaw jigsaws[AC_JIGSAW_COUNT];
    int jigsawCount = ac_get_jigsaws(centerElement, startPos, centerRot, &rng, jigsaws);
    AcPos anchor = startPos;
    int foundAnchor = 0;
    for (int i = 0; i < jigsawCount; i++) {
        if (jigsaws[i].name == AC_ID_MINECRAFT_CITY_ANCHOR) {
            anchor = jigsaws[i].pos;
            foundAnchor = 1;
            break;
        }
    }
    if (!foundAnchor)
        return -1;

    AcPos localAnchor = ac_sub(anchor, startPos);
    AcPos adjusted = ac_sub(startPos, localAnchor);
    AcBox centerBox = ac_element_box(centerElement, adjusted, centerRot);
    int bottomY = adjusted.y;
    int oldAbsoluteGroundY = centerBox.minY + 1;
    adjusted.y += bottomY - oldAbsoluteGroundY;
    centerBox = ac_element_box(centerElement, adjusted, centerRot);

    int centerX = (centerBox.maxX + centerBox.minX) / 2;
    int centerZ = (centerBox.maxZ + centerBox.minZ) / 2;
    int centerY = bottomY + localAnchor.y;

    int globalShape = ac_add_shape(shapes, shapeCount, (AcAabb) {
        centerX - 116.0, ac_max(centerY - 116, -64), centerZ - 116.0,
        centerX + 117.0, ac_min(centerY + 117, 320), centerZ + 117.0,
    });
    if (globalShape < 0 || !ac_free_carve(&shapes[globalShape], centerBox))
        return -1;

    int centerPiece = ac_add_piece(pieces, pieceCount, centerElement, adjusted, centerRot, 0, globalShape);
    if (centerPiece < 0)
        return -1;
    pieces[centerPiece].box = centerBox;

    int queue[AC_MAX_PLACED];
    int queueFirst = 0, queueLast = 0;
    queue[queueLast++] = centerPiece;

    while (queueFirst < queueLast) {
        int sourcePieceId = queue[queueFirst++];
        AcPlacedPiece *sourcePiece = &pieces[sourcePieceId];
        const AcBox sourceBox = sourcePiece->box;
        int sourceFreeShape = -1;

        jigsawCount = ac_get_jigsaws(sourcePiece->elementId, sourcePiece->pos, sourcePiece->rot, &rng, jigsaws);
        for (int sourceJigsawId = 0; sourceJigsawId < jigsawCount; sourceJigsawId++) {
            const AcPlacedJigsaw sourceJigsaw = jigsaws[sourceJigsawId];
            AcPos targetJigsawPos = {
                sourceJigsaw.pos.x + ac_dir_x[sourceJigsaw.front],
                sourceJigsaw.pos.y + ac_dir_y[sourceJigsaw.front],
                sourceJigsaw.pos.z + ac_dir_z[sourceJigsaw.front],
            };
            int sourceJigsawLocalY = sourceJigsaw.pos.y - sourceBox.minY;
            int targetPieces[AC_POOL_ENTRY_COUNT];
            int targetPieceCount = sourcePiece->depth == 7 ? 0 : ac_shuffled_pool(sourceJigsaw.pool, &rng, targetPieces);
            int placed = 0;

            for (int targetPieceIdx = 0; targetPieceIdx < targetPieceCount && !placed; targetPieceIdx++) {
                int targetElement = targetPieces[targetPieceIdx];
                if (ac_elements[targetElement].type == AC_ELEMENT_EMPTY)
                    break;

                int rotations[4] = {0, 1, 2, 3};
                ac_shuffle_ints(rotations, 4, &rng);
                for (int rotIdx = 0; rotIdx < 4 && !placed; rotIdx++) {
                    uint8_t targetRot = rotations[rotIdx];
                    AcPlacedJigsaw targetJigsaws[AC_JIGSAW_COUNT];
                    int targetJigsawCount = ac_get_jigsaws(targetElement, (AcPos) {0, 0, 0}, targetRot, &rng, targetJigsaws);
                    for (int targetJigsawId = 0; targetJigsawId < targetJigsawCount; targetJigsawId++) {
                        const AcPlacedJigsaw targetJigsaw = targetJigsaws[targetJigsawId];
                        if (!ac_can_attach(&sourceJigsaw, &targetJigsaw))
                            continue;

                        AcPos rawTargetBoxPos = ac_sub(targetJigsawPos, targetJigsaw.pos);
                        AcBox rawTargetBox = ac_element_box(targetElement, rawTargetBoxPos, targetRot);
                        int deltaY = sourceJigsawLocalY - targetJigsaw.pos.y + ac_dir_y[sourceJigsaw.front];
                        int targetBoxY = sourceBox.minY + deltaY;
                        int yOffset = targetBoxY - rawTargetBox.minY;
                        AcPos targetBoxPos = {rawTargetBoxPos.x, rawTargetBoxPos.y + yOffset, rawTargetBoxPos.z};
                        AcBox targetBox = ac_move_box(rawTargetBox, 0, yOffset, 0);
                        int childrenFreeShape = sourcePiece->freeShape;

                        if (ac_box_contains_pos(sourceBox, targetJigsawPos)) {
                            if (sourceFreeShape < 0) {
                                sourceFreeShape = ac_add_shape(shapes, shapeCount, ac_aabb_from_box(sourceBox));
                                if (sourceFreeShape < 0)
                                    return -1;
                            }
                            childrenFreeShape = sourceFreeShape;
                        }

                        if (!ac_free_can_place(&shapes[childrenFreeShape], targetBox))
                            continue;
                        if (!ac_free_carve(&shapes[childrenFreeShape], targetBox))
                            return -1;

                        int pieceId = ac_add_piece(pieces, pieceCount, targetElement, targetBoxPos, targetRot, sourcePiece->depth + 1, childrenFreeShape);
                        if (pieceId < 0)
                            return -1;
                        pieces[pieceId].box = targetBox;
                        if (sourcePiece->depth + 1 <= 7) {
                            if (queueLast >= AC_MAX_PLACED)
                                return -1;
                            queue[queueLast++] = pieceId;
                        }
                        placed = 1;
                        break;
                    }
                }
            }
        }
    }

    return 1;
}

static int ac_get_chest_chunk_state(AcChunkLootState *states, int *stateCount, int cx, int cz)
{
    for (int i = 0; i < *stateCount; i++) {
        if (states[i].cx == cx && states[i].cz == cz)
            return i;
    }
    if (*stateCount >= AC_MAX_CHUNKS)
        return -1;
    int idx = (*stateCount)++;
    states[idx] = (AcChunkLootState) {cx, cz, 0};
    return idx;
}

static uint64_t ac_chest_loot_seed(StructureSaltConfig ssconf, int mc, uint64_t seed, int chestX, int chestZ, int skip)
{
    CREATE_RANDOM_SOURCE(rnd, mc <= MC_1_17);
    uint64_t populationSeed = getPopulationSeed(mc, seed, chestX & ~15, chestZ & ~15);
    rnd.setSeed(rnd.state, populationSeed + ssconf.decoratorIndex + 10000 * ssconf.generationStep);
    uint64_t lootSeed = 0;
    for (int i = 0; i <= skip; i++)
        lootSeed = rnd.nextLong(rnd.state);
    return lootSeed;
}

static int ac_fill_piece_chests(Piece *out, const AcPlacedPiece *piece, StructureSaltConfig ssconf, int mc, uint64_t seed, AcChunkLootState *chunkStates, int *chunkStateCount)
{
    const AcElement *element = &ac_elements[piece->elementId];
    int children[4];
    int childCount = 0;
    if (element->type == AC_ELEMENT_SINGLE) {
        children[childCount++] = element->templateId;
    } else if (element->type == AC_ELEMENT_LIST) {
        for (int i = 0; i < element->childCount && childCount < 4; i++)
            children[childCount++] = ac_list_children[element->firstChild + i];
    }

    for (int childIdx = 0; childIdx < childCount; childIdx++) {
        const AcTemplate *template = &ac_templates[children[childIdx]];
        for (int i = 0; i < template->chestCount; i++) {
            if (out->chestCount >= 4)
                return -1;
            const AcChest *chest = &ac_chests[template->chestFirst + i];
            AcPos chestPos = ac_add(ac_transform((AcPos) {chest->x, chest->y, chest->z}, piece->rot), piece->pos);
            int stateIdx = ac_get_chest_chunk_state(chunkStates, chunkStateCount, chestPos.x >> 4, chestPos.z >> 4);
            if (stateIdx < 0)
                return -1;
            int skip = chunkStates[stateIdx].count++;
            int chestIdx = out->chestCount++;
            out->lootTables[chestIdx] = chest->lootTable;
            out->chestPoses[chestIdx] = (Pos) {chestPos.x, chestPos.z};
            out->lootSeeds[chestIdx] = ac_chest_loot_seed(ssconf, mc, seed, chestPos.x, chestPos.z, skip);
        }
    }
    return 1;
}

int getAncientCityLoot(Piece *list, int n, StructureSaltConfig ssconf, int mc, uint64_t seed, int chunkX, int chunkZ)
{
    if (mc < MC_1_21_11)
        return -1;

    AcPlacedPiece *pieces = calloc(AC_MAX_PLACED, sizeof(*pieces));
    AcFreeShape *shapes = calloc(AC_MAX_SHAPES, sizeof(*shapes));
    AcChunkLootState *chunkStates = calloc(AC_MAX_CHUNKS, sizeof(*chunkStates));
    if (!pieces || !shapes || !chunkStates) {
        free(pieces);
        free(shapes);
        free(chunkStates);
        return -1;
    }

    int result = -1;
    int pieceCount = 0, shapeCount = 0;
    if (!ac_generate_pieces(pieces, shapes, &pieceCount, &shapeCount, seed, chunkX, chunkZ))
        goto cleanup;
    if (pieceCount > n)
        goto cleanup;

    int chunkStateCount = 0;
    for (int i = 0; i < pieceCount; i++) {
        const AcPlacedPiece *src = &pieces[i];
        Piece *dst = &list[i];
        memset(dst, 0, sizeof(*dst));
        dst->name = ac_elements[src->elementId].name;
        dst->pos = (Pos3) {src->pos.x, src->pos.y, src->pos.z};
        dst->bb0 = (Pos3) {src->box.minX, src->box.minY, src->box.minZ};
        dst->bb1 = (Pos3) {src->box.maxX, src->box.maxY, src->box.maxZ};
        dst->rot = src->rot;
        dst->depth = src->depth;
        dst->type = src->elementId;
        if (!ac_fill_piece_chests(dst, src, ssconf, mc, seed, chunkStates, &chunkStateCount))
            goto cleanup;
    }

    result = pieceCount;

cleanup:
    free(pieces);
    free(shapes);
    free(chunkStates);
    return result;
}
