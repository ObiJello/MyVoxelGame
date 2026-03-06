#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/AquaticFeatures.h"
#include <vector>

// Reference: net/minecraft/data/worldgen/placement/AquaticPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace ::minecraft::levelgen::placement;

/**
 * AquaticPlacements - Registry of placed aquatic features
 * Reference: AquaticPlacements.java
 *
 * This class creates PlacedFeature instances that combine aquatic
 * ConfiguredFeatures with appropriate placement modifiers.
 */
class AquaticPlacements {
private:
    static bool s_initialized;

public:
    // Seagrass placements
    // Reference: AquaticPlacements.java lines 20-27
    static const PlacedFeature* SEAGRASS_WARM;       // count 80, seagrassShort
    static const PlacedFeature* SEAGRASS_NORMAL;     // count 48, seagrassShort
    static const PlacedFeature* SEAGRASS_COLD;       // count 32, seagrassShort
    static const PlacedFeature* SEAGRASS_RIVER;      // count 48, seagrassSlightlyLessShort
    static const PlacedFeature* SEAGRASS_SWAMP;      // count 64, seagrassMid
    static const PlacedFeature* SEAGRASS_DEEP_WARM;  // count 80, seagrassTall
    static const PlacedFeature* SEAGRASS_DEEP;       // count 48, seagrassTall
    static const PlacedFeature* SEAGRASS_DEEP_COLD;  // count 40, seagrassTall

    // Sea pickle
    // Reference: AquaticPlacements.java line 28
    static const PlacedFeature* SEA_PICKLE;  // RarityFilter(16)

    // Kelp placements
    // Reference: AquaticPlacements.java lines 29-30
    static const PlacedFeature* KELP_COLD;   // NoiseBasedCount(120, 80.0, 0.0)
    static const PlacedFeature* KELP_WARM;   // NoiseBasedCount(80, 80.0, 0.0)

    // Warm ocean vegetation (coral, sea pickle clusters)
    // Reference: AquaticPlacements.java line 31
    static const PlacedFeature* WARM_OCEAN_VEGETATION;  // NoiseBasedCount(20, 400.0, 0.0)

    /**
     * Bootstrap/initialize all aquatic placements
     * Must be called after AquaticFeatures::bootstrap()
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }

private:
    /**
     * Helper to create seagrass placement modifiers
     * Reference: AquaticPlacements.java line 33-35
     * InSquarePlacement.spread(), HEIGHTMAP_TOP_SOLID, CountPlacement.of(count), BiomeFilter.biome()
     */
    static std::vector<PlacementModifier*> seagrassPlacement(int count);
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
