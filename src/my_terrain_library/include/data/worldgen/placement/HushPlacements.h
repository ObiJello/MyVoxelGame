#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/HushFeatures.h"
#include <memory>
#include <string>
#include <vector>

// The Hush — engine-only dimension (DimensionId::Hush). No HushPlacements.java;
// mirrors the NetherPlacements port shape.

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen;
using namespace levelgen::placement;

/**
 * HushPlacements - Registry of The Hush placed features
 *
 *   ORE_ECHO               commonOrePlacement(4, triangle(-32, 32))
 *   ORE_ECHO_DENSE         commonOrePlacement(8, uniform(bottom, 64))     (barrens)
 *   WHISPERWOOD_SPARSE     TREES_PLAINS shape: countExtra(0, 0.05, 1)    (meadows)
 *   WHISPERWOOD_FOREST     CountPlacement::of(10)                       (forest)
 *   PATCH_RESONANCE_BLOOM  noiseThresholdCount(-0.8, 15, 4) + rarity 8
 *   CRYSTAL_FORMATIONS_MEADOWS small formation, rarity 16               (meadows)
 *   CRYSTAL_FORMATIONS_BARRENS field formation, rarity 2                (barrens)
 *   CRYSTAL_SHARDS_BARRENS small formation, CountPlacement::of(1)       (barrens)
 *   HUSH_MOSS_PATCH        CountPlacement::of(2), ocean-floor heightmap
 *
 *   -- Crystal Caverns / surface expansion --
 *   ORE_RESONITE           commonOrePlacement(4, triangle(-64, 0))       (every biome)
 *   ORE_RESONITE_CAVERNS   commonOrePlacement(10, uniform(bottom, 16))   (caverns)
 *   RESONANT_CLUSTERS      CountPlacement::of(48), uniform(bottom, 50)   (caverns)
 *   RESONANT_CRYSTAL_CLUMPS CountPlacement::of(6), uniform(bottom, 50),
 *                          scan down to a floor                          (caverns)
 *   RESONANT_CLUSTER_SURFACE CountPlacement::of(2), surface heightmap    (barrens)
 *   PATCH_HUSH_GRASS_MEADOWS PATCH_GRASS_PLAIN shape x CountPlacement::of(2)
 *   PATCH_HUSH_GRASS_FOREST PATCH_GRASS_FOREST shape: CountPlacement::of(2)
 *   PATCH_RESONANCE_BLOOM_MEADOWS PATCH_RESONANCE_BLOOM at rarity 6
 *   WHISPERWOOD_FOREST     now the 1 : 3 large/normal selector, count 10
 *
 *   -- Sunken Choir / Hollow Deep / Aurora Steppe --
 *   SUNKEN_RUINS           rarity 2, ocean-floor heightmap               (choir)
 *   KELP_SUNKEN_CHOIR      KELP_COLD shape: noiseBasedCount(40, 80, 0)    (choir)
 *   SEA_PICKLE_SUNKEN_CHOIR SEA_PICKLE shape at rarity 8                  (choir)
 *   PATCH_HUSH_GRASS_SHORE PATCH_GRASS_FOREST shape at count 1            (choir)
 *   ROPE_BRIDGES           count 8, motion-blocking heightmap             (deep)
 *   RESONANT_STALACTITES   count 32, uniform(-16, 180), scan up to a
 *                          ceiling, offset -1; no biome filter: the chasm
 *                          walls below the surface band are the caverns'
 *                          biome, and the stalactites belong to the chasm
 *   PATCH_HUSH_GRASS_RIM   PATCH_GRASS_FOREST shape                       (deep)
 *   CRYSTAL_FORMATIONS_RIM tall formation, rarity 4                       (deep)
 *   HUSHSTONE_BOULDERS     FOREST_ROCK list at rarity 3                   (steppe)
 *   PATCH_HUSH_GRASS_STEPPE PATCH_GRASS_PLAIN shape at (-0.8, 8, 14)      (steppe)
 *   PATCH_RESONANCE_BLOOM_STEPPE rarity 12                                (steppe)
 *   CRYSTAL_FORMATIONS_STEPPE tall formation, rarity 8                    (steppe)
 */
class HushPlacements {
private:
    static bool s_initialized;

public:
    static PlacedFeature* ORE_ECHO;
    static PlacedFeature* ORE_ECHO_DENSE;
    static PlacedFeature* WHISPERWOOD_SPARSE;
    static PlacedFeature* WHISPERWOOD_FOREST;
    static PlacedFeature* PATCH_RESONANCE_BLOOM;
    static PlacedFeature* CRYSTAL_FORMATIONS_MEADOWS;
    static PlacedFeature* CRYSTAL_FORMATIONS_BARRENS;
    static PlacedFeature* CRYSTAL_SHARDS_BARRENS;
    static PlacedFeature* HUSH_MOSS_PATCH;

    // Crystal Caverns / surface expansion
    static PlacedFeature* ORE_RESONITE;
    static PlacedFeature* ORE_RESONITE_CAVERNS;
    static PlacedFeature* RESONANT_CLUSTERS;
    static PlacedFeature* RESONANT_CRYSTAL_CLUMPS;
    static PlacedFeature* RESONANT_CLUSTER_SURFACE;
    static PlacedFeature* PATCH_HUSH_GRASS_MEADOWS;
    static PlacedFeature* PATCH_HUSH_GRASS_FOREST;
    static PlacedFeature* PATCH_RESONANCE_BLOOM_MEADOWS;

    // Sunken Choir / Hollow Deep / Aurora Steppe
    static PlacedFeature* SUNKEN_RUINS;
    static PlacedFeature* KELP_SUNKEN_CHOIR;
    static PlacedFeature* SEA_PICKLE_SUNKEN_CHOIR;
    static PlacedFeature* PATCH_HUSH_GRASS_SHORE;
    static PlacedFeature* ROPE_BRIDGES;
    static PlacedFeature* RESONANT_STALACTITES;
    static PlacedFeature* PATCH_HUSH_GRASS_RIM;
    static PlacedFeature* CRYSTAL_FORMATIONS_RIM;
    static PlacedFeature* HUSHSTONE_BOULDERS;
    static PlacedFeature* PATCH_HUSH_GRASS_STEPPE;
    static PlacedFeature* PATCH_RESONANCE_BLOOM_STEPPE;
    static PlacedFeature* CRYSTAL_FORMATIONS_STEPPE;

    /**
     * Bootstrap/initialize all Hush placements
     * Bootstraps HushFeatures first if needed.
     */
    static void bootstrap();

    static bool isInitialized() { return s_initialized; }
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
