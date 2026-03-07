#include "data/worldgen/features/TreeFeatures.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "util/IntProvider.h"

// Reference: net/minecraft/data/worldgen/features/TreeFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using levelgen::feature::TreeFeature;
using levelgen::ConfiguredFeature;
using levelgen::ConfiguredFeatureImpl;
using levelgen::feature::configurations::TreeConfiguration;
using levelgen::feature::configurations::TreeConfigurationBuilder;
using levelgen::feature::trunkplacers::StraightTrunkPlacer;
using levelgen::feature::trunkplacers::ForkingTrunkPlacer;
using levelgen::feature::trunkplacers::FancyTrunkPlacer;
using levelgen::feature::trunkplacers::DarkOakTrunkPlacer;
using levelgen::feature::trunkplacers::GiantTrunkPlacer;
using levelgen::feature::trunkplacers::MegaJungleTrunkPlacer;
using levelgen::feature::trunkplacers::CherryTrunkPlacer;
using levelgen::feature::foliageplacers::BlobFoliagePlacer;
using levelgen::feature::foliageplacers::SpruceFoliagePlacer;
using levelgen::feature::foliageplacers::PineFoliagePlacer;
using levelgen::feature::foliageplacers::FancyFoliagePlacer;
using levelgen::feature::foliageplacers::AcaciaFoliagePlacer;
using levelgen::feature::foliageplacers::DarkOakFoliagePlacer;
using levelgen::feature::foliageplacers::BushFoliagePlacer;
using levelgen::feature::foliageplacers::MegaJungleFoliagePlacer;
using levelgen::feature::foliageplacers::MegaPineFoliagePlacer;
using levelgen::feature::foliageplacers::CherryFoliagePlacer;
using levelgen::feature::featuresize::TwoLayersFeatureSize;
using levelgen::feature::featuresize::ThreeLayersFeatureSize;
using levelgen::feature::featuresize::FeatureSize;
using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::WeightedStateProvider;
using levelgen::feature::stateproviders::RandomizedIntStateProvider;
using levelgen::feature::stateproviders::WeightedStateEntry;
using levelgen::feature::stateproviders::SimpleStateProvider;
namespace treedecorators = levelgen::feature::treedecorators;
using levelgen::feature::treedecorators::TreeDecorator;
using levelgen::feature::treedecorators::PlaceOnGroundDecorator;
using levelgen::feature::treedecorators::TrunkVineDecorator;
using levelgen::feature::treedecorators::CocoaDecorator;
using levelgen::feature::treedecorators::AttachedToLogsDecorator;
using levelgen::feature::treedecorators::AlterGroundDecorator;
using levelgen::feature::treedecorators::PaleMossDecorator;
using levelgen::feature::treedecorators::LeaveVineDecorator;
using levelgen::feature::treedecorators::AttachedToLeavesDecorator;
using levelgen::feature::treedecorators::BeehiveDecorator;
using levelgen::feature::trunkplacers::UpwardsBranchingTrunkPlacer;
using levelgen::feature::foliageplacers::RandomSpreadFoliagePlacer;
using levelgen::feature::rootplacers::MangroveRootPlacer;
using levelgen::feature::rootplacers::AboveRootPlacement;
using levelgen::FallenTreeConfiguration;
using levelgen::FallenTreeFeature;
// Foliage placers use levelgen::carver::IntProvider
using levelgen::carver::IntProvider;
using levelgen::carver::ConstantInt;
using levelgen::carver::UniformInt;

// Static members
bool TreeFeatures::s_initialized = false;
std::shared_ptr<TreeFeature> TreeFeatures::s_treeFeature = nullptr;

// ConfiguredFeature pointers - Basic trees
ConfiguredFeature* TreeFeatures::OAK = nullptr;
ConfiguredFeature* TreeFeatures::BIRCH = nullptr;
ConfiguredFeature* TreeFeatures::SPRUCE = nullptr;
ConfiguredFeature* TreeFeatures::PINE = nullptr;
ConfiguredFeature* TreeFeatures::JUNGLE_TREE = nullptr;
ConfiguredFeature* TreeFeatures::ACACIA = nullptr;
ConfiguredFeature* TreeFeatures::DARK_OAK = nullptr;
ConfiguredFeature* TreeFeatures::FANCY_OAK = nullptr;
ConfiguredFeature* TreeFeatures::PALE_OAK = nullptr;
ConfiguredFeature* TreeFeatures::PALE_OAK_CREAKING = nullptr;
ConfiguredFeature* TreeFeatures::MANGROVE = nullptr;
ConfiguredFeature* TreeFeatures::TALL_MANGROVE = nullptr;

// ConfiguredFeature pointers - Mega trees
ConfiguredFeature* TreeFeatures::MEGA_SPRUCE = nullptr;
ConfiguredFeature* TreeFeatures::MEGA_PINE = nullptr;
ConfiguredFeature* TreeFeatures::MEGA_JUNGLE_TREE = nullptr;

// ConfiguredFeature pointers - Special trees
ConfiguredFeature* TreeFeatures::SWAMP_OAK = nullptr;
ConfiguredFeature* TreeFeatures::JUNGLE_BUSH = nullptr;

// ConfiguredFeature pointers - Nether fungi
ConfiguredFeature* TreeFeatures::CRIMSON_FUNGUS = nullptr;
ConfiguredFeature* TreeFeatures::WARPED_FUNGUS = nullptr;

// ConfiguredFeature pointers - Leaf litter variants (no bees)
ConfiguredFeature* TreeFeatures::OAK_LEAF_LITTER = nullptr;
ConfiguredFeature* TreeFeatures::BIRCH_LEAF_LITTER = nullptr;
ConfiguredFeature* TreeFeatures::DARK_OAK_LEAF_LITTER = nullptr;
ConfiguredFeature* TreeFeatures::FANCY_OAK_LEAF_LITTER = nullptr;

// ConfiguredFeature pointers - Bees variants
ConfiguredFeature* TreeFeatures::OAK_BEES_0002_LEAF_LITTER = nullptr;
ConfiguredFeature* TreeFeatures::OAK_BEES_002 = nullptr;
ConfiguredFeature* TreeFeatures::BIRCH_BEES_0002 = nullptr;
ConfiguredFeature* TreeFeatures::BIRCH_BEES_0002_LEAF_LITTER = nullptr;
ConfiguredFeature* TreeFeatures::BIRCH_BEES_002 = nullptr;
ConfiguredFeature* TreeFeatures::FANCY_OAK_BEES_0002_LEAF_LITTER = nullptr;
ConfiguredFeature* TreeFeatures::FANCY_OAK_BEES_002 = nullptr;
ConfiguredFeature* TreeFeatures::FANCY_OAK_BEES = nullptr;
ConfiguredFeature* TreeFeatures::SUPER_BIRCH_BEES_0002 = nullptr;
ConfiguredFeature* TreeFeatures::SUPER_BIRCH_BEES = nullptr;

// ConfiguredFeature pointers - Fallen trees
ConfiguredFeature* TreeFeatures::FALLEN_OAK = nullptr;
ConfiguredFeature* TreeFeatures::FALLEN_OAK_TREE = nullptr;
ConfiguredFeature* TreeFeatures::FALLEN_BIRCH = nullptr;
ConfiguredFeature* TreeFeatures::FALLEN_BIRCH_TREE = nullptr;
ConfiguredFeature* TreeFeatures::FALLEN_SUPER_BIRCH_TREE = nullptr;
ConfiguredFeature* TreeFeatures::FALLEN_SPRUCE_TREE = nullptr;
ConfiguredFeature* TreeFeatures::FALLEN_JUNGLE_TREE = nullptr;

// ConfiguredFeature pointers - Cherry trees
ConfiguredFeature* TreeFeatures::CHERRY = nullptr;
ConfiguredFeature* TreeFeatures::CHERRY_BEES_005 = nullptr;

// Storage for ConfiguredFeatureImpl instances
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<TreeConfiguration>> s_configs;
static std::vector<std::unique_ptr<FallenTreeConfiguration>> s_fallenTreeConfigs;

// Storage for shared_ptr objects (keep alive)
static std::vector<std::shared_ptr<BlockStateProvider>> s_providers;
static std::vector<std::shared_ptr<TrunkPlacer>> s_trunkPlacers;
static std::vector<std::shared_ptr<FoliagePlacer>> s_foliagePlacers;
static std::vector<std::shared_ptr<FeatureSize>> s_featureSizes;
static std::vector<std::shared_ptr<IntProvider>> s_intProviders;
static std::vector<std::shared_ptr<treedecorators::TreeDecorator>> s_decorators;
static std::vector<std::shared_ptr<levelgen::feature::rootplacers::RootPlacer>> s_rootPlacers;

// FallenTreeFeature instance
static std::unique_ptr<FallenTreeFeature> s_fallenTreeFeature;

// Helper to create shared IntProvider
static std::shared_ptr<IntProvider> constantInt(int32_t value) {
    auto ptr = std::make_shared<ConstantInt>(value);
    s_intProviders.push_back(ptr);
    return ptr;
}

static std::shared_ptr<IntProvider> uniformInt(int32_t min, int32_t max) {
    auto ptr = std::make_shared<UniformInt>(min, max);
    s_intProviders.push_back(ptr);
    return ptr;
}

TreeConfigurationBuilder TreeFeatures::createStraightBlobTree(
    const std::string& logBlock,
    const std::string& leavesBlock,
    int baseHeight,
    int heightRandA,
    int heightRandB,
    int blobRadius
) {
    // Reference: TreeFeatures.java createStraightBlobTree lines 121-122
    auto trunkProvider = BlockStateProvider::simple(logBlock);
    auto foliageProvider = BlockStateProvider::simple(leavesBlock);
    s_providers.push_back(trunkProvider);
    s_providers.push_back(foliageProvider);

    auto trunkPlacer = std::make_shared<StraightTrunkPlacer>(baseHeight, heightRandA, heightRandB);
    s_trunkPlacers.push_back(trunkPlacer);

    auto foliagePlacer = std::make_shared<BlobFoliagePlacer>(
        constantInt(blobRadius),
        constantInt(0),
        3
    );
    s_foliagePlacers.push_back(foliagePlacer);

    auto featureSize = std::make_shared<TwoLayersFeatureSize>(1, 0, 1);
    s_featureSizes.push_back(featureSize);

    return TreeConfigurationBuilder(
        trunkProvider,
        trunkPlacer,
        foliageProvider,
        foliagePlacer,
        featureSize
    );
}

TreeConfigurationBuilder TreeFeatures::createOak() {
    // Reference: TreeFeatures.java createOak line 125-126
    // OAK: StraightTrunkPlacer(4, 2, 0), BlobFoliagePlacer(2, 0, 3), TwoLayersFeatureSize(1, 0, 1)
    return createStraightBlobTree("minecraft:oak_log", "minecraft:oak_leaves", 4, 2, 0, 2).ignoreVines();
}

TreeConfigurationBuilder TreeFeatures::createBirch() {
    // Reference: TreeFeatures.java createBirch line 153-154
    // BIRCH: StraightTrunkPlacer(5, 2, 0), BlobFoliagePlacer(2, 0, 3), TwoLayersFeatureSize(1, 0, 1)
    return createStraightBlobTree("minecraft:birch_log", "minecraft:birch_leaves", 5, 2, 0, 2).ignoreVines();
}

TreeConfigurationBuilder TreeFeatures::createSuperBirch() {
    // Reference: TreeFeatures.java createSuperBirch line 157-158
    // SUPER_BIRCH: StraightTrunkPlacer(5, 2, 6), BlobFoliagePlacer(2, 0, 3), TwoLayersFeatureSize(1, 0, 1)
    return createStraightBlobTree("minecraft:birch_log", "minecraft:birch_leaves", 5, 2, 6, 2).ignoreVines();
}

TreeConfigurationBuilder TreeFeatures::createJungleTree() {
    // Reference: TreeFeatures.java createJungleTree line 161-162
    // JUNGLE_TREE: StraightTrunkPlacer(4, 8, 0), BlobFoliagePlacer(2, 0, 3)
    return createStraightBlobTree("minecraft:jungle_log", "minecraft:jungle_leaves", 4, 8, 0, 2);
}

TreeConfigurationBuilder TreeFeatures::createFancyOak() {
    // Reference: TreeFeatures.java createFancyOak lines 165-166
    auto trunkProvider = BlockStateProvider::simple("minecraft:oak_log");
    auto foliageProvider = BlockStateProvider::simple("minecraft:oak_leaves");
    s_providers.push_back(trunkProvider);
    s_providers.push_back(foliageProvider);

    auto trunkPlacer = std::make_shared<FancyTrunkPlacer>(3, 11, 0);
    s_trunkPlacers.push_back(trunkPlacer);

    auto foliagePlacer = std::make_shared<FancyFoliagePlacer>(
        constantInt(2),
        constantInt(4),
        4
    );
    s_foliagePlacers.push_back(foliagePlacer);

    auto featureSize = std::make_shared<TwoLayersFeatureSize>(0, 0, 0, std::optional<int>(4));
    s_featureSizes.push_back(featureSize);

    return TreeConfigurationBuilder(
        trunkProvider,
        trunkPlacer,
        foliageProvider,
        foliagePlacer,
        featureSize
    ).ignoreVines();
}

TreeConfigurationBuilder TreeFeatures::createDarkOak() {
    // Reference: TreeFeatures.java createDarkOak lines 129-130
    auto trunkProvider = BlockStateProvider::simple("minecraft:dark_oak_log");
    auto foliageProvider = BlockStateProvider::simple("minecraft:dark_oak_leaves");
    s_providers.push_back(trunkProvider);
    s_providers.push_back(foliageProvider);

    auto trunkPlacer = std::make_shared<DarkOakTrunkPlacer>(6, 2, 1);
    s_trunkPlacers.push_back(trunkPlacer);

    auto foliagePlacer = std::make_shared<DarkOakFoliagePlacer>(
        constantInt(0),
        constantInt(0)
    );
    s_foliagePlacers.push_back(foliagePlacer);

    auto featureSize = std::make_shared<ThreeLayersFeatureSize>(1, 1, 0, 1, 2, std::nullopt);
    s_featureSizes.push_back(featureSize);

    return TreeConfigurationBuilder(
        trunkProvider,
        trunkPlacer,
        foliageProvider,
        foliagePlacer,
        featureSize
    );
}

TreeConfigurationBuilder TreeFeatures::cherry() {
    // Reference: TreeFeatures.java cherry() lines 169-170
    // CherryTrunkPlacer(7, 1, 0, WeightedListInt({1,2,3}), UniformInt(2,4), UniformInt(-4,-3), UniformInt(-1,0))
    // CherryFoliagePlacer(ConstantInt(4), ConstantInt(0), ConstantInt(5), 0.25F, 0.5F, 0.16666667F, 0.33333334F)
    // TwoLayersFeatureSize(1, 0, 2)

    auto trunkProvider = BlockStateProvider::simple("minecraft:cherry_log");
    auto foliageProvider = BlockStateProvider::simple("minecraft:cherry_leaves");
    s_providers.push_back(trunkProvider);
    s_providers.push_back(foliageProvider);

    // Branch count: Java uses WeightedListInt with 1, 2, 3 equally weighted
    // Reference: new WeightedListInt(WeightedList.builder().add(ConstantInt.of(1), 1).add(ConstantInt.of(2), 1).add(ConstantInt.of(3), 1).build())
    auto branchCount = levelgen::carver::WeightedListInt::builder()
        .add(std::make_shared<levelgen::carver::ConstantInt>(1), 1)
        .add(std::make_shared<levelgen::carver::ConstantInt>(2), 1)
        .add(std::make_shared<levelgen::carver::ConstantInt>(3), 1)
        .buildShared();
    s_intProviders.push_back(branchCount);
    auto branchHorizontalLength = uniformInt(2, 4);
    auto branchEndOffsetFromTop = uniformInt(-1, 0);

    // CherryTrunkPlacer takes separate min/max for branchStartOffsetFromTop
    auto trunkPlacer = std::make_shared<CherryTrunkPlacer>(
        7, 1, 0,       // baseHeight, heightRandA, heightRandB
        branchCount,
        branchHorizontalLength,
        -4, -3,        // branchStartOffsetFromTop min/max
        branchEndOffsetFromTop
    );
    s_trunkPlacers.push_back(trunkPlacer);

    auto foliagePlacer = std::make_shared<CherryFoliagePlacer>(
        constantInt(4),    // radius
        constantInt(0),    // offset
        constantInt(5),    // height
        0.25f,             // wideBottomLayerHoleChance
        0.5f,              // cornerHoleChance
        0.16666667f,       // hangingLeavesChance
        0.33333334f        // hangingLeavesExtensionChance
    );
    s_foliagePlacers.push_back(foliagePlacer);

    auto featureSize = std::make_shared<TwoLayersFeatureSize>(1, 0, 2);
    s_featureSizes.push_back(featureSize);

    return TreeConfigurationBuilder(
        trunkProvider,
        trunkPlacer,
        foliageProvider,
        foliagePlacer,
        featureSize
    ).ignoreVines();
}

void TreeFeatures::bootstrap() {
    if (s_initialized) return;

    // Create shared TreeFeature instance
    s_treeFeature = std::make_shared<TreeFeature>();

    // Helper to register a tree
    auto registerTree = [](TreeConfigurationBuilder& builder) -> ConfiguredFeature* {
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        ConfiguredFeature* ptr = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
        return ptr;
    };

    // =========================================================================
    // BASIC TREES
    // Reference: TreeFeatures.java bootstrap lines 189-199
    // =========================================================================

    // OAK
    {
        auto builder = createOak();
        OAK = registerTree(builder);
    }

    // BIRCH
    {
        auto builder = createBirch();
        BIRCH = registerTree(builder);
    }

    // SPRUCE
    // Reference: TreeFeatures.java line 198
    // StraightTrunkPlacer(5, 2, 1), SpruceFoliagePlacer(UniformInt(2,3), UniformInt(0,2), UniformInt(1,2))
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:spruce_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:spruce_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<StraightTrunkPlacer>(5, 2, 1);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<SpruceFoliagePlacer>(
            uniformInt(2, 3),
            uniformInt(0, 2),
            uniformInt(1, 2)
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(2, 0, 2);
        s_featureSizes.push_back(featureSize);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).ignoreVines().build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        SPRUCE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PINE
    // Reference: TreeFeatures.java line 199
    // StraightTrunkPlacer(6, 4, 0), PineFoliagePlacer(ConstantInt(1), ConstantInt(1), UniformInt(3,4))
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:spruce_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:spruce_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<StraightTrunkPlacer>(6, 4, 0);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<PineFoliagePlacer>(
            constantInt(1),
            constantInt(1),
            uniformInt(3, 4)
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(2, 0, 2);
        s_featureSizes.push_back(featureSize);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).ignoreVines().build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        PINE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // JUNGLE_TREE (with vines)
    {
        auto builder = createJungleTree();
        auto cocoaDecorator = std::make_shared<CocoaDecorator>(0.2f);
        auto trunkVineDecorator = std::make_shared<TrunkVineDecorator>();
        auto leaveVineDecorator = std::make_shared<LeaveVineDecorator>(0.25f);
        s_decorators.push_back(cocoaDecorator);
        s_decorators.push_back(trunkVineDecorator);
        s_decorators.push_back(leaveVineDecorator);
        builder.decorators({cocoaDecorator, trunkVineDecorator, leaveVineDecorator}).ignoreVines();
        JUNGLE_TREE = registerTree(builder);
    }

    // ACACIA
    // Reference: TreeFeatures.java line 195
    // ForkingTrunkPlacer(5, 2, 2), AcaciaFoliagePlacer(ConstantInt(2), ConstantInt(0))
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:acacia_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:acacia_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<ForkingTrunkPlacer>(5, 2, 2);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<AcaciaFoliagePlacer>(
            constantInt(2),
            constantInt(0)
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(1, 0, 2);
        s_featureSizes.push_back(featureSize);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).ignoreVines().build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        ACACIA = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // DARK_OAK
    {
        auto builder = createDarkOak();
        builder.ignoreVines();
        DARK_OAK = registerTree(builder);
    }

    // PALE_OAK
    // Reference: TreeFeatures.java line 191
    // DarkOakTrunkPlacer(6, 2, 1), DarkOakFoliagePlacer(0, 0), ThreeLayersFeatureSize(1, 1, 0, 1, 2)
    // Decorators: PaleMossDecorator(0.15F, 0.4F, 0.8F)
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:pale_oak_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:pale_oak_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<DarkOakTrunkPlacer>(6, 2, 1);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<DarkOakFoliagePlacer>(
            constantInt(0),
            constantInt(0)
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<ThreeLayersFeatureSize>(1, 1, 0, 1, 2, std::nullopt);
        s_featureSizes.push_back(featureSize);

        // PaleMossDecorator(leavesProbability=0.15f, trunkProbability=0.4f, groundProbability=0.8f)
        auto paleMossDecorator = std::make_shared<PaleMossDecorator>(0.15f, 0.4f, 0.8f);
        s_decorators.push_back(paleMossDecorator);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).decorators({paleMossDecorator}).ignoreVines().build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        PALE_OAK = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // MANGROVE
    // Reference: TreeFeatures.java line 211
    // UpwardsBranchingTrunkPlacer(2, 1, 4, UniformInt(1, 4), 0.5F, UniformInt(0, 1))
    // RandomSpreadFoliagePlacer(ConstantInt(3), ConstantInt(0), ConstantInt(2), 70)
    // MangroveRootPlacer(UniformInt(1, 3), mangrove_roots, Optional(AboveRootPlacement(moss_carpet, 0.5)), maxRootWidth=8, maxRootLength=15, randomSkewChance=0.2)
    // TwoLayersFeatureSize(2, 0, 2)
    // Decorators: LeaveVineDecorator(0.125F), AttachedToLeavesDecorator, BeehiveDecorator(0.01F)
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:mangrove_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:mangrove_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        // UpwardsBranchingTrunkPlacer(2, 1, 4, UniformInt(1, 4), 0.5F, UniformInt(0, 1))
        auto trunkPlacer = std::make_shared<UpwardsBranchingTrunkPlacer>(
            2, 1, 4,            // baseHeight, heightRandA, heightRandB
            uniformInt(1, 4),   // extraBranchSteps
            0.5f,               // placeBranchPerLogProbability
            uniformInt(0, 1)    // extraBranchLength
        );
        s_trunkPlacers.push_back(trunkPlacer);

        // RandomSpreadFoliagePlacer(ConstantInt(3), ConstantInt(0), ConstantInt(2), 70)
        auto foliagePlacer = std::make_shared<RandomSpreadFoliagePlacer>(
            constantInt(3),     // radius
            constantInt(0),     // offset
            constantInt(2),     // foliageHeight
            70                  // leafPlacementAttempts
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(2, 0, 2);
        s_featureSizes.push_back(featureSize);

        // MangroveRootPlacer configuration
        auto rootProvider = BlockStateProvider::simple("minecraft:mangrove_roots");
        s_providers.push_back(rootProvider);

        auto mossCarpetProvider = BlockStateProvider::simple("minecraft:moss_carpet");
        s_providers.push_back(mossCarpetProvider);

        auto rootPlacer = std::make_shared<MangroveRootPlacer>(
            uniformInt(1, 3),                                    // trunkOffsetY
            rootProvider,                                        // rootProvider
            std::make_optional(AboveRootPlacement(mossCarpetProvider, 0.5f)), // aboveRootPlacement
            8,                                                   // maxRootWidth
            15,                                                  // maxRootLength
            0.2f                                                 // randomSkewChance
        );
        s_rootPlacers.push_back(rootPlacer);

        // Decorators
        auto leaveVineDecorator = std::make_shared<LeaveVineDecorator>(0.125f);
        s_decorators.push_back(leaveVineDecorator);

        BlockState* hangingPropaguleState = minecraft::world::level::block::Blocks::MANGROVE_PROPAGULE
            ? minecraft::world::level::block::Blocks::MANGROVE_PROPAGULE->defaultBlockState()
            : minecraft::world::level::block::Blocks::getDefaultState("minecraft:mangrove_propagule");
        if (hangingPropaguleState) {
            hangingPropaguleState = hangingPropaguleState->trySetValue(
                *minecraft::world::level::block::state::properties::BlockStateProperties::HANGING,
                true
            );
        }
        auto hangingPropaguleProvider = std::make_shared<RandomizedIntStateProvider>(
            BlockStateProvider::simple(hangingPropaguleState),
            "age",
            0,
            4
        );
        s_providers.push_back(hangingPropaguleProvider);
        auto attachedToLeavesDecorator = std::make_shared<AttachedToLeavesDecorator>(
            0.14f,
            1,
            0,
            hangingPropaguleProvider,
            2,
            std::vector<core::Direction>{core::Direction::DOWN}
        );
        s_decorators.push_back(attachedToLeavesDecorator);

        // BeehiveDecorator with 1% chance
        auto beehive001 = std::make_shared<BeehiveDecorator>(0.01f);
        s_decorators.push_back(beehive001);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                rootPlacer,
                featureSize
            ).decorators({leaveVineDecorator, attachedToLeavesDecorator, beehive001}).ignoreVines().build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        MANGROVE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // TALL_MANGROVE
    // Reference: TreeFeatures.java line 212
    // UpwardsBranchingTrunkPlacer(4, 1, 9, UniformInt(1, 6), 0.5F, UniformInt(0, 1))
    // RandomSpreadFoliagePlacer(ConstantInt(3), ConstantInt(0), ConstantInt(2), 70)
    // MangroveRootPlacer(UniformInt(3, 7), ..., maxRootWidth=8, maxRootLength=15, randomSkewChance=0.2)
    // TwoLayersFeatureSize(3, 0, 2)
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:mangrove_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:mangrove_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        // UpwardsBranchingTrunkPlacer(4, 1, 9, UniformInt(1, 6), 0.5F, UniformInt(0, 1))
        auto trunkPlacer = std::make_shared<UpwardsBranchingTrunkPlacer>(
            4, 1, 9,            // baseHeight, heightRandA, heightRandB
            uniformInt(1, 6),   // extraBranchSteps
            0.5f,               // placeBranchPerLogProbability
            uniformInt(0, 1)    // extraBranchLength
        );
        s_trunkPlacers.push_back(trunkPlacer);

        // RandomSpreadFoliagePlacer(ConstantInt(3), ConstantInt(0), ConstantInt(2), 70)
        auto foliagePlacer = std::make_shared<RandomSpreadFoliagePlacer>(
            constantInt(3),     // radius
            constantInt(0),     // offset
            constantInt(2),     // foliageHeight
            70                  // leafPlacementAttempts
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(3, 0, 2);
        s_featureSizes.push_back(featureSize);

        // MangroveRootPlacer configuration
        auto rootProvider = BlockStateProvider::simple("minecraft:mangrove_roots");
        s_providers.push_back(rootProvider);

        auto mossCarpetProvider = BlockStateProvider::simple("minecraft:moss_carpet");
        s_providers.push_back(mossCarpetProvider);

        auto rootPlacer = std::make_shared<MangroveRootPlacer>(
            uniformInt(3, 7),                                    // trunkOffsetY (higher for tall mangrove)
            rootProvider,                                        // rootProvider
            std::make_optional(AboveRootPlacement(mossCarpetProvider, 0.5f)), // aboveRootPlacement
            8,                                                   // maxRootWidth
            15,                                                  // maxRootLength
            0.2f                                                 // randomSkewChance
        );
        s_rootPlacers.push_back(rootPlacer);

        // Decorators (same as MANGROVE)
        auto leaveVineDecorator = std::make_shared<LeaveVineDecorator>(0.125f);
        s_decorators.push_back(leaveVineDecorator);

        BlockState* hangingPropaguleState = minecraft::world::level::block::Blocks::MANGROVE_PROPAGULE
            ? minecraft::world::level::block::Blocks::MANGROVE_PROPAGULE->defaultBlockState()
            : minecraft::world::level::block::Blocks::getDefaultState("minecraft:mangrove_propagule");
        if (hangingPropaguleState) {
            hangingPropaguleState = hangingPropaguleState->trySetValue(
                *minecraft::world::level::block::state::properties::BlockStateProperties::HANGING,
                true
            );
        }
        auto hangingPropaguleProvider = std::make_shared<RandomizedIntStateProvider>(
            BlockStateProvider::simple(hangingPropaguleState),
            "age",
            0,
            4
        );
        s_providers.push_back(hangingPropaguleProvider);
        auto attachedToLeavesDecorator = std::make_shared<AttachedToLeavesDecorator>(
            0.14f,
            1,
            0,
            hangingPropaguleProvider,
            2,
            std::vector<core::Direction>{core::Direction::DOWN}
        );
        s_decorators.push_back(attachedToLeavesDecorator);

        auto beehive001 = std::make_shared<BeehiveDecorator>(0.01f);
        s_decorators.push_back(beehive001);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                rootPlacer,
                featureSize
            ).decorators({leaveVineDecorator, attachedToLeavesDecorator, beehive001}).ignoreVines().build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        TALL_MANGROVE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FANCY_OAK
    {
        auto builder = createFancyOak();
        FANCY_OAK = registerTree(builder);
    }

    // =========================================================================
    // CHERRY TREES
    // Reference: TreeFeatures.java lines 196-197
    // =========================================================================

    // CHERRY
    {
        auto builder = cherry();
        CHERRY = registerTree(builder);
    }

    // CHERRY_BEES_005
    // For now, same as CHERRY (skip bee decorator)
    {
        auto builder = cherry();
        CHERRY_BEES_005 = registerTree(builder);
    }

    // =========================================================================
    // MEGA TREES
    // Reference: TreeFeatures.java lines 203-205
    // =========================================================================

    // MEGA_SPRUCE
    // GiantTrunkPlacer(13, 2, 14), MegaPineFoliagePlacer(ConstantInt(0), ConstantInt(0), UniformInt(13,17))
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:spruce_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:spruce_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<GiantTrunkPlacer>(13, 2, 14);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<MegaPineFoliagePlacer>(
            constantInt(0),
            constantInt(0),
            uniformInt(13, 17)
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(1, 1, 2);
        s_featureSizes.push_back(featureSize);

        auto podzolProvider = BlockStateProvider::simple("minecraft:podzol");
        s_providers.push_back(podzolProvider);
        auto alterGroundDecorator = std::make_shared<AlterGroundDecorator>(podzolProvider);
        s_decorators.push_back(alterGroundDecorator);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).decorators({alterGroundDecorator}).build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        MEGA_SPRUCE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // MEGA_PINE
    // GiantTrunkPlacer(13, 2, 14), MegaPineFoliagePlacer(ConstantInt(0), ConstantInt(0), UniformInt(3,7))
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:spruce_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:spruce_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<GiantTrunkPlacer>(13, 2, 14);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<MegaPineFoliagePlacer>(
            constantInt(0),
            constantInt(0),
            uniformInt(3, 7)
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(1, 1, 2);
        s_featureSizes.push_back(featureSize);

        auto podzolProvider = BlockStateProvider::simple("minecraft:podzol");
        s_providers.push_back(podzolProvider);
        auto alterGroundDecorator = std::make_shared<AlterGroundDecorator>(podzolProvider);
        s_decorators.push_back(alterGroundDecorator);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).decorators({alterGroundDecorator}).build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        MEGA_PINE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // MEGA_JUNGLE_TREE
    // MegaJungleTrunkPlacer(10, 2, 19), MegaJungleFoliagePlacer(ConstantInt(2), ConstantInt(0), 2)
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:jungle_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:jungle_leaves");
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<MegaJungleTrunkPlacer>(10, 2, 19);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<MegaJungleFoliagePlacer>(
            constantInt(2),
            constantInt(0),
            2
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(1, 1, 2);
        s_featureSizes.push_back(featureSize);

        auto trunkVineDecorator = std::make_shared<TrunkVineDecorator>();
        auto leaveVineDecorator = std::make_shared<LeaveVineDecorator>(0.25f);
        s_decorators.push_back(trunkVineDecorator);
        s_decorators.push_back(leaveVineDecorator);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).decorators({trunkVineDecorator, leaveVineDecorator}).build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        MEGA_JUNGLE_TREE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SPECIAL TREES
    // Reference: TreeFeatures.java lines 208-209
    // =========================================================================

    // SWAMP_OAK
    // StraightTrunkPlacer(5, 3, 0), BlobFoliagePlacer(3, 0, 3), with vine decorator
    {
        auto builder = createStraightBlobTree("minecraft:oak_log", "minecraft:oak_leaves", 5, 3, 0, 3);
        auto leaveVineDecorator = std::make_shared<LeaveVineDecorator>(0.25f);
        s_decorators.push_back(leaveVineDecorator);
        builder.decorators({leaveVineDecorator});
        SWAMP_OAK = registerTree(builder);
    }

    // JUNGLE_BUSH
    // StraightTrunkPlacer(1, 0, 0), BushFoliagePlacer(ConstantInt(2), ConstantInt(1), 2)
    {
        auto trunkProvider = BlockStateProvider::simple("minecraft:jungle_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:oak_leaves");  // Uses oak leaves!
        s_providers.push_back(trunkProvider);
        s_providers.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<StraightTrunkPlacer>(1, 0, 0);
        s_trunkPlacers.push_back(trunkPlacer);

        auto foliagePlacer = std::make_shared<BushFoliagePlacer>(
            constantInt(2),
            constantInt(1),
            2
        );
        s_foliagePlacers.push_back(foliagePlacer);

        auto featureSize = std::make_shared<TwoLayersFeatureSize>(0, 0, 0);
        s_featureSizes.push_back(featureSize);

        auto config = std::make_unique<TreeConfiguration>(
            TreeConfigurationBuilder(
                trunkProvider,
                trunkPlacer,
                foliageProvider,
                foliagePlacer,
                featureSize
            ).build()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        JUNGLE_BUSH = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // LEAF LITTER DECORATORS
    // Reference: TreeFeatures.java lines 187-188
    // Reference: VegetationFeatures.java leafLitterPatchBuilder()
    // =========================================================================

    // Create leaf litter block states for WeightedStateProvider
    // Java's leafLitterPatchBuilder creates states with SEGMENT_AMOUNT (1-4) and FACING (N/S/E/W)
    // Each state has weight 1, so sparse (1-3) = 12 states, thick (1-4) = 16 states
    // CRITICAL: WeightedStateProvider calls nextInt(totalWeight) per getState() call!
    auto leafLitter = minecraft::world::level::block::Blocks::LEAF_LITTER;

    // Directions in Java's Plane.HORIZONTAL order: NORTH, SOUTH, WEST, EAST
    std::vector<core::Direction> horizontalDirs = {
        core::Direction::NORTH,
        core::Direction::SOUTH,
        core::Direction::WEST,
        core::Direction::EAST
    };

    // Build sparse leaf litter states (amounts 1-3, 4 directions = 12 states, totalWeight=12)
    // Java order: for amount in [min..max]: for dir in [NORTH, SOUTH, WEST, EAST]
    std::vector<WeightedStateEntry> sparseLeafLitterStates;
    for (int amount = 1; amount <= 3; ++amount) {
        for (auto dir : horizontalDirs) {
            // Get properly configured state with amount and facing
            BlockState* state = leafLitter->getState(amount, dir);
            sparseLeafLitterStates.push_back({state, 1});
        }
    }
    auto sparseLeafLitterProvider = std::make_shared<WeightedStateProvider>(sparseLeafLitterStates);
    s_providers.push_back(sparseLeafLitterProvider);

    // Build thick leaf litter states (amounts 1-4, 4 directions = 16 states, totalWeight=16)
    std::vector<WeightedStateEntry> thickLeafLitterStates;
    for (int amount = 1; amount <= 4; ++amount) {
        for (auto dir : horizontalDirs) {
            // Get properly configured state with amount and facing
            BlockState* state = leafLitter->getState(amount, dir);
            thickLeafLitterStates.push_back({state, 1});
        }
    }
    auto thickLeafLitterProvider = std::make_shared<WeightedStateProvider>(thickLeafLitterStates);
    s_providers.push_back(thickLeafLitterProvider);

    // sparseLeafLitter: tries=96, radius=4, height=2
    auto sparseLeafLitter = std::make_shared<PlaceOnGroundDecorator>(
        96, 4, 2, sparseLeafLitterProvider
    );
    s_decorators.push_back(sparseLeafLitter);

    // thickLeafLitter: tries=150, radius=2, height=2
    auto thickLeafLitter = std::make_shared<PlaceOnGroundDecorator>(
        150, 2, 2, thickLeafLitterProvider
    );
    s_decorators.push_back(thickLeafLitter);

    // =========================================================================
    // LEAF LITTER TREE VARIANTS
    // Reference: TreeFeatures.java lines 224-227
    // =========================================================================

    // OAK_LEAF_LITTER
    {
        auto builder = createOak();
        builder.decorators({sparseLeafLitter, thickLeafLitter});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        OAK_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // BIRCH_LEAF_LITTER
    {
        auto builder = createBirch();
        builder.decorators({sparseLeafLitter, thickLeafLitter});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        BIRCH_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // DARK_OAK_LEAF_LITTER
    {
        auto builder = createDarkOak();
        builder.decorators({sparseLeafLitter, thickLeafLitter});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        DARK_OAK_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FANCY_OAK_LEAF_LITTER
    {
        auto builder = createFancyOak();
        builder.decorators({sparseLeafLitter, thickLeafLitter});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        FANCY_OAK_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // BEES + LEAF LITTER TREE VARIANTS
    // Reference: TreeFeatures.java lines 213, 217, 220
    // These have beehive decorator (0.002F) + leaf litter decorators
    // =========================================================================

    // Create beehive0002 decorator (0.2% chance)
    auto beehive0002 = std::make_shared<treedecorators::BeehiveDecorator>(0.002f);
    s_decorators.push_back(beehive0002);

    // Create beehive002 decorator (2% chance)
    auto beehive002 = std::make_shared<treedecorators::BeehiveDecorator>(0.02f);
    s_decorators.push_back(beehive002);

    // Create beehive005 decorator (5% chance)
    auto beehive005 = std::make_shared<treedecorators::BeehiveDecorator>(0.05f);
    s_decorators.push_back(beehive005);

    // OAK_BEES_0002_LEAF_LITTER
    // Reference: createOak().decorators(List.of(beehive0002, sparseLeafLitter, thickLeafLitter))
    {
        auto builder = createOak();
        builder.decorators({beehive0002, sparseLeafLitter, thickLeafLitter});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        OAK_BEES_0002_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // BIRCH_BEES_0002
    // Reference: createBirch().decorators(List.of(beehive0002))
    {
        auto builder = createBirch();
        builder.decorators({beehive0002});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        BIRCH_BEES_0002 = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // BIRCH_BEES_0002_LEAF_LITTER
    // Reference: createBirch().decorators(List.of(beehive0002, sparseLeafLitter, thickLeafLitter))
    {
        auto builder = createBirch();
        builder.decorators({beehive0002, sparseLeafLitter, thickLeafLitter});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        BIRCH_BEES_0002_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FANCY_OAK_BEES_0002_LEAF_LITTER
    // Reference: createFancyOak().decorators(List.of(beehive0002, sparseLeafLitter, thickLeafLitter))
    {
        auto builder = createFancyOak();
        builder.decorators({beehive0002, sparseLeafLitter, thickLeafLitter});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        FANCY_OAK_BEES_0002_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // FALLEN TREES
    // Reference: TreeFeatures.java FALLEN_OAK, FALLEN_BIRCH
    // =========================================================================

    // Create FallenTreeFeature instance
    s_fallenTreeFeature = std::make_unique<FallenTreeFeature>();

    // TrunkVineDecorator for stump
    auto trunkVineDecorator = std::make_shared<TrunkVineDecorator>();
    s_decorators.push_back(trunkVineDecorator);

    // AttachedToLogsDecorator for mushrooms on fallen logs
    // Reference: Uses 10% probability, UP direction, weighted mushroom provider
    auto mushroomProvider = std::make_shared<WeightedStateProvider>(
        std::vector<WeightedStateEntry>{
            {minecraft::world::level::block::Blocks::RED_MUSHROOM->defaultBlockState(), 2},
            {minecraft::world::level::block::Blocks::BROWN_MUSHROOM->defaultBlockState(), 1}
        }
    );
    s_providers.push_back(mushroomProvider);

    auto attachedToLogsDecorator = std::make_shared<AttachedToLogsDecorator>(
        0.1f,  // 10% probability
        mushroomProvider,  // blockProvider
        std::vector<core::Direction>{core::Direction::UP}  // directions
    );
    s_decorators.push_back(attachedToLogsDecorator);

    // FALLEN_OAK
    // Reference: logLength UniformInt(4,7)
    {
        auto oakLogProvider = std::make_shared<SimpleStateProvider>(
            minecraft::world::level::block::Blocks::OAK_LOG->defaultBlockState()
        );
        s_providers.push_back(oakLogProvider);

        // Use util::UniformInt for FallenTreeConfiguration (not levelgen::carver::UniformInt)
        auto logLength = std::make_shared<util::UniformInt>(4, 7);

        auto config = std::make_unique<FallenTreeConfiguration>(
            oakLogProvider,
            logLength,
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{trunkVineDecorator},
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{attachedToLogsDecorator}
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<FallenTreeConfiguration, FallenTreeFeature>>(
            s_fallenTreeFeature.get(), *config);
        FALLEN_OAK = feature.get();
        s_fallenTreeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FALLEN_BIRCH
    // Reference: logLength UniformInt(5,8)
    {
        auto birchLogProvider = std::make_shared<SimpleStateProvider>(
            minecraft::world::level::block::Blocks::BIRCH_LOG->defaultBlockState()
        );
        s_providers.push_back(birchLogProvider);

        // Use util::UniformInt for FallenTreeConfiguration (not levelgen::carver::UniformInt)
        auto logLength = std::make_shared<util::UniformInt>(5, 8);

        auto config = std::make_unique<FallenTreeConfiguration>(
            birchLogProvider,
            logLength,
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{trunkVineDecorator},
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{attachedToLogsDecorator}
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<FallenTreeConfiguration, FallenTreeFeature>>(
            s_fallenTreeFeature.get(), *config);
        FALLEN_BIRCH = feature.get();
        s_fallenTreeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // ADDITIONAL BEES VARIANTS
    // Reference: TreeFeatures.java
    // =========================================================================

    // OAK_BEES_002 - oak with 2% bee chance (no leaf litter)
    // Reference: createOak().decorators(List.of(beehive002))
    {
        auto builder = createOak();
        builder.decorators({beehive002});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        OAK_BEES_002 = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // BIRCH_BEES_002 - birch with 2% bee chance (no leaf litter)
    // Reference: createBirch().decorators(List.of(beehive002))
    {
        auto builder = createBirch();
        builder.decorators({beehive002});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        BIRCH_BEES_002 = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FANCY_OAK_BEES_002 - fancy oak with 2% bee chance (no leaf litter)
    // Reference: createFancyOak().decorators(List.of(beehive002))
    {
        auto builder = createFancyOak();
        builder.decorators({beehive002});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        FANCY_OAK_BEES_002 = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FANCY_OAK_BEES - fancy oak with 5% bee chance
    // Reference: createFancyOak().decorators(List.of(beehive005))
    {
        auto builder = createFancyOak();
        builder.decorators({beehive005});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        FANCY_OAK_BEES = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // SUPER_BIRCH_BEES_0002 - super birch with 0.02% bee chance
    // Reference: createSuperBirch().decorators(List.of(beehive0002))
    {
        auto builder = createSuperBirch();
        builder.decorators({beehive0002});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        SUPER_BIRCH_BEES_0002 = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // SUPER_BIRCH_BEES - super birch with 5% bee chance
    // Reference: createSuperBirch().decorators(List.of(beehive005))
    {
        auto builder = createSuperBirch();
        builder.decorators({beehive005});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        SUPER_BIRCH_BEES = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // CHERRY TREES
    // Reference: TreeFeatures.java CHERRY, CHERRY_BEES_005
    // =========================================================================

    // CHERRY - basic cherry tree
    {
        auto builder = cherry();
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        CHERRY = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // CHERRY_BEES_005 - cherry with 5% bee chance
    {
        auto builder = cherry();
        builder.decorators({beehive005});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        CHERRY_BEES_005 = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // ADDITIONAL FALLEN TREES
    // Reference: TreeFeatures.java
    // =========================================================================

    // FALLEN_OAK_TREE - alias for FALLEN_OAK (used by some placements)
    FALLEN_OAK_TREE = FALLEN_OAK;

    // FALLEN_BIRCH_TREE - alias for FALLEN_BIRCH
    FALLEN_BIRCH_TREE = FALLEN_BIRCH;

    // FALLEN_SUPER_BIRCH_TREE
    {
        auto birchLogProvider = std::make_shared<SimpleStateProvider>(
            minecraft::world::level::block::Blocks::BIRCH_LOG->defaultBlockState()
        );
        s_providers.push_back(birchLogProvider);

        auto logLength = std::make_shared<util::UniformInt>(6, 10);

        auto config = std::make_unique<FallenTreeConfiguration>(
            birchLogProvider,
            logLength,
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{trunkVineDecorator},
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{attachedToLogsDecorator}
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<FallenTreeConfiguration, FallenTreeFeature>>(
            s_fallenTreeFeature.get(), *config);
        FALLEN_SUPER_BIRCH_TREE = feature.get();
        s_fallenTreeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FALLEN_SPRUCE_TREE
    {
        auto spruceLogProvider = std::make_shared<SimpleStateProvider>(
            minecraft::world::level::block::Blocks::SPRUCE_LOG->defaultBlockState()
        );
        s_providers.push_back(spruceLogProvider);

        auto logLength = std::make_shared<util::UniformInt>(4, 8);

        auto config = std::make_unique<FallenTreeConfiguration>(
            spruceLogProvider,
            logLength,
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{trunkVineDecorator},
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{attachedToLogsDecorator}
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<FallenTreeConfiguration, FallenTreeFeature>>(
            s_fallenTreeFeature.get(), *config);
        FALLEN_SPRUCE_TREE = feature.get();
        s_fallenTreeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FALLEN_JUNGLE_TREE
    {
        auto jungleLogProvider = std::make_shared<SimpleStateProvider>(
            minecraft::world::level::block::Blocks::JUNGLE_LOG->defaultBlockState()
        );
        s_providers.push_back(jungleLogProvider);

        auto logLength = std::make_shared<util::UniformInt>(5, 9);

        auto config = std::make_unique<FallenTreeConfiguration>(
            jungleLogProvider,
            logLength,
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{trunkVineDecorator},
            std::vector<std::shared_ptr<treedecorators::TreeDecorator>>{attachedToLogsDecorator}
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<FallenTreeConfiguration, FallenTreeFeature>>(
            s_fallenTreeFeature.get(), *config);
        FALLEN_JUNGLE_TREE = feature.get();
        s_fallenTreeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
