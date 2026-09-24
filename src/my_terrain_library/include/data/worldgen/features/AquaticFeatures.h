#pragma once

#include "levelgen/feature/Feature.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <vector>

// Reference: net/minecraft/data/worldgen/features/AquaticFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;

/**
 * AquaticFeatures - Registry of configured aquatic features
 * Reference: AquaticFeatures.java
 *
 * This class creates ConfiguredFeature instances for kelp, seagrass, etc.
 */
class AquaticFeatures {
private:
    static bool s_initialized;

public:
    // =========================================================================
    // SEAGRASS - Reference: AquaticFeatures.java lines 15-18
    // =========================================================================
    static levelgen::ConfiguredFeature* SEAGRASS_SHORT;             // 30% tall
    static levelgen::ConfiguredFeature* SEAGRASS_SLIGHTLY_LESS_SHORT; // 40% tall
    static levelgen::ConfiguredFeature* SEAGRASS_MID;               // 60% tall
    static levelgen::ConfiguredFeature* SEAGRASS_TALL;              // 80% tall

    // =========================================================================
    // SEA PICKLE - Reference: AquaticFeatures.java line 19
    // =========================================================================
    static levelgen::ConfiguredFeature* SEA_PICKLE;                 // one pickle, 1-4

    // =========================================================================
    // KELP - Reference: AquaticFeatures.java line 20
    // =========================================================================
    static levelgen::ConfiguredFeature* KELP;                       // block column

    // =========================================================================
    // WARM OCEAN - Reference: AquaticFeatures.java line 21
    // =========================================================================
    static levelgen::ConfiguredFeature* WARM_OCEAN_VEGETATION;      // coral features

    /**
     * Bootstrap/initialize all aquatic features
     * Must be called before using any features
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
