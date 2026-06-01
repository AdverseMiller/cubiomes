#ifndef ANCIENT_CITY_H_
#define ANCIENT_CITY_H_

#include "../finders.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generate ancient city jigsaw pieces and fill loot table data for loot chests.
 *
 * This currently targets the 1.21.11+/26.1.2 ancient city template and pool
 * layout. The output buffer should be large enough for the generated jigsaw
 * tree; 400 entries is sufficient for normal use, matching SeedMapper's
 * existing large-structure allocation.
 */
int getAncientCityLoot(Piece *list, int n, StructureSaltConfig ssconf, int mc, uint64_t seed, int chunkX, int chunkZ);

#ifdef __cplusplus
}
#endif

#endif // ANCIENT_CITY_H_
