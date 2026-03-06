#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/CaveFeatures.h"
#include <vector>

// Reference: net/minecraft/data/worldgen/placement/CavePlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace ::minecraft::levelgen::placement;

/**
 * CavePlacements - Registry of placed cave features
 * Reference: CavePlacements.java
 *
 * This class creates PlacedFeature instances that combine cave
 * ConfiguredFeatures with appropriate placement modifiers.
 */
class CavePlacements {
private:
    static bool s_initialized;

public:
    // =========================================================================
    // MONSTER ROOMS (DUNGEONS) - Reference: CavePlacements.java lines 29-30
    // =========================================================================
    static const PlacedFeature* MONSTER_ROOM;
    static const PlacedFeature* MONSTER_ROOM_DEEP;

    // =========================================================================
    // FOSSILS - Reference: CavePlacements.java lines 31-32
    // =========================================================================
    static const PlacedFeature* FOSSIL_UPPER;
    static const PlacedFeature* FOSSIL_LOWER;

    // =========================================================================
    // DRIPSTONE - Reference: CavePlacements.java lines 33-35
    // =========================================================================
    static const PlacedFeature* DRIPSTONE_CLUSTER;
    static const PlacedFeature* LARGE_DRIPSTONE;
    static const PlacedFeature* POINTED_DRIPSTONE;

    // =========================================================================
    // UNDERWATER FEATURES - Reference: CavePlacements.java line 36
    // =========================================================================
    static const PlacedFeature* UNDERWATER_MAGMA;

    // =========================================================================
    // GLOW LICHEN - Reference: CavePlacements.java line 37
    // =========================================================================
    static const PlacedFeature* GLOW_LICHEN;

    // =========================================================================
    // LUSH CAVES - Reference: CavePlacements.java lines 38-44
    // =========================================================================
    static const PlacedFeature* ROOTED_AZALEA_TREE;
    static const PlacedFeature* CAVE_VINES;
    static const PlacedFeature* LUSH_CAVES_VEGETATION;
    static const PlacedFeature* LUSH_CAVES_CLAY;
    static const PlacedFeature* LUSH_CAVES_CEILING_VEGETATION;
    static const PlacedFeature* SPORE_BLOSSOM;

    // =========================================================================
    // CLASSIC VINES - Reference: CavePlacements.java line 44
    // =========================================================================
    static const PlacedFeature* CLASSIC_VINES;

    // =========================================================================
    // AMETHYST GEODE - Reference: CavePlacements.java line 45
    // =========================================================================
    static const PlacedFeature* AMETHYST_GEODE;

    // =========================================================================
    // SCULK - Reference: CavePlacements.java lines 46-48
    // =========================================================================
    static const PlacedFeature* SCULK_PATCH_DEEP_DARK;
    static const PlacedFeature* SCULK_PATCH_ANCIENT_CITY;
    static const PlacedFeature* SCULK_VEIN;

    /**
     * Bootstrap/initialize all cave placements
     * Must be called after CaveFeatures::bootstrap()
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
