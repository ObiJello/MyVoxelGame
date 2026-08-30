#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/NetherFeatures.h"
#include <vector>
#include <memory>

// Reference: net/minecraft/data/worldgen/placement/NetherPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen;
using namespace levelgen::placement;

/**
 * NetherPlacements - Registry of placed nether features
 * Reference: NetherPlacements.java
 */
class NetherPlacements {
private:
    static bool s_initialized;

public:
    static PlacedFeature* DELTA;
    static PlacedFeature* SMALL_BASALT_COLUMNS;
    static PlacedFeature* LARGE_BASALT_COLUMNS;
    static PlacedFeature* BASALT_BLOBS;
    static PlacedFeature* BLACKSTONE_BLOBS;
    static PlacedFeature* GLOWSTONE_EXTRA;
    static PlacedFeature* GLOWSTONE;
    static PlacedFeature* CRIMSON_FOREST_VEGETATION;
    static PlacedFeature* WARPED_FOREST_VEGETATION;
    static PlacedFeature* NETHER_SPROUTS;
    static PlacedFeature* TWISTING_VINES;
    static PlacedFeature* WEEPING_VINES;
    static PlacedFeature* PATCH_CRIMSON_ROOTS;
    static PlacedFeature* BASALT_PILLAR;
    static PlacedFeature* SPRING_DELTA;
    static PlacedFeature* SPRING_CLOSED;
    static PlacedFeature* SPRING_CLOSED_DOUBLE;
    static PlacedFeature* SPRING_OPEN;
    static PlacedFeature* PATCH_SOUL_FIRE;
    static PlacedFeature* PATCH_FIRE;

    /**
     * Bootstrap/initialize all nether placements
     * Must be called after NetherFeatures::bootstrap()
     */
    static void bootstrap();

    static bool isInitialized() { return s_initialized; }
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
