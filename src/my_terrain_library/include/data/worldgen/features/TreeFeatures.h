#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/feature/TreeFeature.h"
#include "levelgen/feature/configurations/TreeConfiguration.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/featuresize/FeatureSize.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/carver/CarverConfiguration.h"

// Reference: net/minecraft/data/worldgen/features/TreeFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen::feature;
using namespace levelgen::feature::configurations;
using namespace levelgen::feature::trunkplacers;
using namespace levelgen::feature::foliageplacers;
using namespace levelgen::feature::featuresize;
using namespace levelgen::feature::stateproviders;

/**
 * TreeFeatures - Registry of configured tree features
 * Reference: TreeFeatures.java
 */
class TreeFeatures {
private:
    static bool s_initialized;

    // Tree feature instance (shared)
    static std::shared_ptr<TreeFeature> s_treeFeature;

public:
    // =========================================================================
    // BASIC TREES - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* OAK;
    static levelgen::ConfiguredFeature* BIRCH;
    static levelgen::ConfiguredFeature* SPRUCE;
    static levelgen::ConfiguredFeature* PINE;
    static levelgen::ConfiguredFeature* JUNGLE_TREE;
    static levelgen::ConfiguredFeature* ACACIA;
    static levelgen::ConfiguredFeature* DARK_OAK;
    static levelgen::ConfiguredFeature* FANCY_OAK;
    static levelgen::ConfiguredFeature* PALE_OAK;
    static levelgen::ConfiguredFeature* PALE_OAK_CREAKING;
    static levelgen::ConfiguredFeature* MANGROVE;
    static levelgen::ConfiguredFeature* TALL_MANGROVE;

    // =========================================================================
    // MEGA TREES - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* MEGA_SPRUCE;
    static levelgen::ConfiguredFeature* MEGA_PINE;
    static levelgen::ConfiguredFeature* MEGA_JUNGLE_TREE;

    // =========================================================================
    // SPECIAL TREES - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* SWAMP_OAK;
    static levelgen::ConfiguredFeature* JUNGLE_BUSH;
    static levelgen::ConfiguredFeature* AZALEA_TREE;

    // =========================================================================
    // NETHER FUNGI - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* CRIMSON_FUNGUS;
    static levelgen::ConfiguredFeature* WARPED_FUNGUS;

    // =========================================================================
    // LEAF LITTER VARIANTS (no bees) - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* OAK_LEAF_LITTER;
    static levelgen::ConfiguredFeature* BIRCH_LEAF_LITTER;
    static levelgen::ConfiguredFeature* DARK_OAK_LEAF_LITTER;
    static levelgen::ConfiguredFeature* FANCY_OAK_LEAF_LITTER;

    // =========================================================================
    // BEES VARIANTS - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* OAK_BEES_0002_LEAF_LITTER;
    static levelgen::ConfiguredFeature* OAK_BEES_002;
    static levelgen::ConfiguredFeature* BIRCH_BEES_0002;
    static levelgen::ConfiguredFeature* BIRCH_BEES_0002_LEAF_LITTER;
    static levelgen::ConfiguredFeature* BIRCH_BEES_002;
    static levelgen::ConfiguredFeature* FANCY_OAK_BEES_0002_LEAF_LITTER;
    static levelgen::ConfiguredFeature* FANCY_OAK_BEES_002;
    static levelgen::ConfiguredFeature* FANCY_OAK_BEES;
    static levelgen::ConfiguredFeature* SUPER_BIRCH_BEES_0002;
    static levelgen::ConfiguredFeature* SUPER_BIRCH_BEES;

    // =========================================================================
    // FALLEN TREES - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* FALLEN_OAK;
    static levelgen::ConfiguredFeature* FALLEN_OAK_TREE;
    static levelgen::ConfiguredFeature* FALLEN_BIRCH;
    static levelgen::ConfiguredFeature* FALLEN_BIRCH_TREE;
    static levelgen::ConfiguredFeature* FALLEN_SUPER_BIRCH_TREE;
    static levelgen::ConfiguredFeature* FALLEN_SPRUCE_TREE;
    static levelgen::ConfiguredFeature* FALLEN_JUNGLE_TREE;

    // =========================================================================
    // CHERRY TREES - Reference: TreeFeatures.java
    // =========================================================================
    static levelgen::ConfiguredFeature* CHERRY;
    static levelgen::ConfiguredFeature* CHERRY_BEES_005;

    /**
     * Bootstrap/initialize all tree features
     */
    static void bootstrap();

    /**
     * Check if bootstrapped
     */
    static bool isInitialized() { return s_initialized; }

private:
    /**
     * Helper: Create straight trunk + blob foliage tree
     * Reference: TreeFeatures.java createStraightBlobTree
     */
    static TreeConfigurationBuilder createStraightBlobTree(
        const std::string& logBlock,
        const std::string& leavesBlock,
        int baseHeight,
        int heightRandA,
        int heightRandB,
        int blobRadius
    );

    /**
     * Helper: Create oak configuration
     * Reference: TreeFeatures.java createOak
     */
    static TreeConfigurationBuilder createOak();

    /**
     * Helper: Create birch configuration
     * Reference: TreeFeatures.java createBirch
     */
    static TreeConfigurationBuilder createBirch();

    /**
     * Helper: Create super birch configuration
     * Reference: TreeFeatures.java createSuperBirch
     */
    static TreeConfigurationBuilder createSuperBirch();

    /**
     * Helper: Create jungle tree configuration
     * Reference: TreeFeatures.java createJungleTree
     */
    static TreeConfigurationBuilder createJungleTree();

    /**
     * Helper: Create fancy oak configuration
     * Reference: TreeFeatures.java createFancyOak
     */
    static TreeConfigurationBuilder createFancyOak();

    /**
     * Helper: Create dark oak configuration
     * Reference: TreeFeatures.java createDarkOak
     */
    static TreeConfigurationBuilder createDarkOak();

    /**
     * Helper: Create cherry configuration
     * Reference: TreeFeatures.java cherry
     */
    static TreeConfigurationBuilder cherry();
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
