#pragma once

#include "levelgen/feature/Feature.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <vector>

// Reference: 26.3 net/minecraft/data/worldgen/features/NetherFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

/**
 * NetherFeatures - Registry of nether configured features
 * Reference: 26.3 NetherFeatures.java
 */
class NetherFeatures {
private:
    static bool s_initialized;

public:
    // Configured features - Reference: NetherFeatures.java bootstrap()
    static levelgen::ConfiguredFeature* DELTA;
    static levelgen::ConfiguredFeature* SMALL_BASALT_COLUMNS;
    static levelgen::ConfiguredFeature* LARGE_BASALT_COLUMNS;
    static levelgen::ConfiguredFeature* BASALT_BLOBS;
    static levelgen::ConfiguredFeature* BLACKSTONE_BLOBS;
    static levelgen::ConfiguredFeature* GLOWSTONE_EXTRA;
    static levelgen::ConfiguredFeature* CRIMSON_FOREST_VEGETATION;
    static levelgen::ConfiguredFeature* WARPED_FOREST_VEGETATION;
    static levelgen::ConfiguredFeature* NETHER_SPROUTS;
    static levelgen::ConfiguredFeature* TWISTING_VINES;
    static levelgen::ConfiguredFeature* WEEPING_VINES;
    static levelgen::ConfiguredFeature* CRIMSON_ROOTS;
    static levelgen::ConfiguredFeature* BASALT_PILLAR;
    static levelgen::ConfiguredFeature* SPRING_LAVA_NETHER;
    static levelgen::ConfiguredFeature* SPRING_NETHER_CLOSED;
    static levelgen::ConfiguredFeature* SPRING_NETHER_OPEN;
    static levelgen::ConfiguredFeature* FIRE;
    static levelgen::ConfiguredFeature* SOUL_FIRE;

    /**
     * Bootstrap/initialize all nether configured features
     */
    static void bootstrap();

    static bool isInitialized() { return s_initialized; }
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
