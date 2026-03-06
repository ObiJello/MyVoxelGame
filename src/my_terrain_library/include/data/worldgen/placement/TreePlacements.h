#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/TreeFeatures.h"
#include <vector>

// Reference: net/minecraft/data/worldgen/placement/TreePlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace ::minecraft::levelgen::placement;

/**
 * TreePlacements - Registry of placed tree features
 * Reference: TreePlacements.java
 *
 * This class creates PlacedFeature instances that combine tree
 * ConfiguredFeatures with appropriate placement modifiers.
 * Most use filteredByBlockSurvival(sapling) pattern.
 */
class TreePlacements {
private:
    static bool s_initialized;

public:
    // =========================================================================
    // NETHER FUNGI - Reference: TreePlacements.java lines 22-23
    // =========================================================================
    static const PlacedFeature* CRIMSON_FUNGI;
    static const PlacedFeature* WARPED_FUNGI;

    // =========================================================================
    // BASIC CHECKED TREES - Reference: TreePlacements.java lines 24-36
    // All use PlacementUtils.filteredByBlockSurvival(sapling)
    // =========================================================================
    static const PlacedFeature* OAK_CHECKED;
    static const PlacedFeature* DARK_OAK_CHECKED;
    static const PlacedFeature* PALE_OAK_CHECKED;
    static const PlacedFeature* PALE_OAK_CREAKING_CHECKED;
    static const PlacedFeature* BIRCH_CHECKED;
    static const PlacedFeature* ACACIA_CHECKED;
    static const PlacedFeature* SPRUCE_CHECKED;
    static const PlacedFeature* MANGROVE_CHECKED;
    static const PlacedFeature* CHERRY_CHECKED;
    static const PlacedFeature* PINE_ON_SNOW;
    static const PlacedFeature* SPRUCE_ON_SNOW;
    static const PlacedFeature* PINE_CHECKED;
    static const PlacedFeature* JUNGLE_TREE_CHECKED;
    static const PlacedFeature* FANCY_OAK_CHECKED;
    static const PlacedFeature* MEGA_JUNGLE_TREE_CHECKED;
    static const PlacedFeature* MEGA_SPRUCE_CHECKED;
    static const PlacedFeature* MEGA_PINE_CHECKED;
    static const PlacedFeature* TALL_MANGROVE_CHECKED;
    static const PlacedFeature* JUNGLE_BUSH;

    // =========================================================================
    // BIRCH WITH BEES - Reference: TreePlacements.java lines 43-44
    // =========================================================================
    static const PlacedFeature* SUPER_BIRCH_BEES_0002;
    static const PlacedFeature* SUPER_BIRCH_BEES;

    // =========================================================================
    // OAK WITH BEES - Reference: TreePlacements.java lines 45-46
    // =========================================================================
    static const PlacedFeature* OAK_BEES_0002_LEAF_LITTER;
    static const PlacedFeature* OAK_BEES_002;

    // =========================================================================
    // BIRCH WITH BEES (regular) - Reference: TreePlacements.java lines 47-49
    // =========================================================================
    static const PlacedFeature* BIRCH_BEES_0002_PLACED;
    static const PlacedFeature* BIRCH_BEES_0002_LEAF_LITTER;
    static const PlacedFeature* BIRCH_BEES_002;

    // =========================================================================
    // FANCY OAK WITH BEES - Reference: TreePlacements.java lines 50-52
    // =========================================================================
    static const PlacedFeature* FANCY_OAK_BEES_0002_LEAF_LITTER;
    static const PlacedFeature* FANCY_OAK_BEES_002;
    static const PlacedFeature* FANCY_OAK_BEES;

    // =========================================================================
    // CHERRY WITH BEES - Reference: TreePlacements.java line 53
    // =========================================================================
    static const PlacedFeature* CHERRY_BEES_005;

    // =========================================================================
    // LEAF LITTER VARIANTS - Reference: TreePlacements.java lines 54-57
    // =========================================================================
    static const PlacedFeature* OAK_LEAF_LITTER;
    static const PlacedFeature* DARK_OAK_LEAF_LITTER;
    static const PlacedFeature* BIRCH_LEAF_LITTER;
    static const PlacedFeature* FANCY_OAK_LEAF_LITTER;

    // =========================================================================
    // FALLEN TREES - Reference: TreePlacements.java lines 58-62
    // =========================================================================
    static const PlacedFeature* FALLEN_OAK_TREE;
    static const PlacedFeature* FALLEN_BIRCH_TREE;
    static const PlacedFeature* FALLEN_SUPER_BIRCH_TREE;
    static const PlacedFeature* FALLEN_SPRUCE_TREE;
    static const PlacedFeature* FALLEN_JUNGLE_TREE;

    /**
     * Bootstrap/initialize all tree placements
     * Must be called after TreeFeatures::bootstrap()
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }

    /**
     * Helper: filteredByBlockSurvival - creates placement that checks if sapling can survive
     * Reference: PlacementUtils.filteredByBlockSurvival()
     */
    static std::vector<PlacementModifier*> filteredByBlockSurvival();
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
