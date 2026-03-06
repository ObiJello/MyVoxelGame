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
    // Static feature instances
    static levelgen::SeagrassFeature s_seagrassFeature;
    static levelgen::KelpFeature s_kelpFeature;
    static levelgen::SeaPickleFeature s_seaPickleFeature;
    static levelgen::SimpleRandomSelectorFeature s_simpleRandomSelectorFeature;

    static bool s_initialized;

public:
    // =========================================================================
    // SEAGRASS - Reference: AquaticFeatures.java lines 15-18
    // =========================================================================
    static levelgen::ConfiguredFeature* SEAGRASS_SHORT;             // probability 0.3
    static levelgen::ConfiguredFeature* SEAGRASS_SLIGHTLY_LESS_SHORT; // probability 0.4
    static levelgen::ConfiguredFeature* SEAGRASS_MID;               // probability 0.6
    static levelgen::ConfiguredFeature* SEAGRASS_TALL;              // probability 0.8

    // =========================================================================
    // SEA PICKLE - Reference: AquaticFeatures.java line 19
    // =========================================================================
    static levelgen::ConfiguredFeature* SEA_PICKLE;                 // CountConfiguration(20)

    // =========================================================================
    // KELP - Reference: AquaticFeatures.java line 20
    // =========================================================================
    static levelgen::ConfiguredFeature* KELP;                       // no config

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
