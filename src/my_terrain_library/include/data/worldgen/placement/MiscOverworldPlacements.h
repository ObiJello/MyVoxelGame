#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/MiscOverworldFeatures.h"
#include <vector>

// Reference: net/minecraft/data/worldgen/placement/MiscOverworldPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace ::minecraft::levelgen::placement;

/**
 * MiscOverworldPlacements - Registry of placed miscellaneous overworld features
 * Reference: MiscOverworldPlacements.java
 */
class MiscOverworldPlacements {
private:
    static bool s_initialized;

public:
    // =========================================================================
    // ICE FEATURES - Reference: MiscOverworldPlacements.java lines 33-38
    // =========================================================================
    static const PlacedFeature* ICE_SPIKE;
    static const PlacedFeature* ICE_PATCH;
    static const PlacedFeature* ICEBERG_PACKED;
    static const PlacedFeature* ICEBERG_BLUE;
    static const PlacedFeature* BLUE_ICE;

    // =========================================================================
    // FOREST ROCK - Reference: MiscOverworldPlacements.java line 35
    // =========================================================================
    static const PlacedFeature* FOREST_ROCK;

    // =========================================================================
    // LAKES - Reference: MiscOverworldPlacements.java lines 39-40
    // =========================================================================
    static const PlacedFeature* LAKE_LAVA_UNDERGROUND;
    static const PlacedFeature* LAKE_LAVA_SURFACE;

    // =========================================================================
    // DISK PLACEMENTS - Reference: MiscOverworldPlacements.java lines 41-44
    // =========================================================================
    static const PlacedFeature* DISK_CLAY;
    static const PlacedFeature* DISK_GRAVEL;
    static const PlacedFeature* DISK_SAND;
    static const PlacedFeature* DISK_GRASS;

    // =========================================================================
    // TOP LAYER / VOID - Reference: MiscOverworldPlacements.java lines 45-46
    // =========================================================================
    static const PlacedFeature* FREEZE_TOP_LAYER;
    static const PlacedFeature* VOID_START_PLATFORM;

    // =========================================================================
    // DESERT WELL - Reference: MiscOverworldPlacements.java line 47
    // =========================================================================
    static const PlacedFeature* DESERT_WELL;

    // =========================================================================
    // SPRINGS - Reference: MiscOverworldPlacements.java lines 48-50
    // =========================================================================
    static const PlacedFeature* SPRING_LAVA;
    static const PlacedFeature* SPRING_LAVA_FROZEN;
    static const PlacedFeature* SPRING_WATER;

    /**
     * Bootstrap/initialize all miscellaneous overworld placements
     * Must be called after MiscOverworldFeatures::bootstrap()
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
