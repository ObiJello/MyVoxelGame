#include "data/worldgen/features/NetherFeatures.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "util/IntProvider.h"
#include <deque>
#include <set>
#include <string>

// Reference: 26.3 net/minecraft/data/worldgen/features/NetherFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using namespace levelgen::placement;
using levelgen::blockpredicates::BlockPredicate;
using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::RandomizedIntStateProvider;
using levelgen::feature::stateproviders::SimpleStateProvider;
using levelgen::feature::stateproviders::WeightedStateEntry;
using levelgen::feature::stateproviders::WeightedStateProvider;

bool NetherFeatures::s_initialized = false;

// Configured feature pointers
ConfiguredFeature* NetherFeatures::DELTA = nullptr;
ConfiguredFeature* NetherFeatures::SMALL_BASALT_COLUMNS = nullptr;
ConfiguredFeature* NetherFeatures::LARGE_BASALT_COLUMNS = nullptr;
ConfiguredFeature* NetherFeatures::BASALT_BLOBS = nullptr;
ConfiguredFeature* NetherFeatures::BLACKSTONE_BLOBS = nullptr;
ConfiguredFeature* NetherFeatures::GLOWSTONE_EXTRA = nullptr;
ConfiguredFeature* NetherFeatures::CRIMSON_FOREST_VEGETATION = nullptr;
ConfiguredFeature* NetherFeatures::WARPED_FOREST_VEGETATION = nullptr;
ConfiguredFeature* NetherFeatures::NETHER_SPROUTS = nullptr;
ConfiguredFeature* NetherFeatures::TWISTING_VINES = nullptr;
ConfiguredFeature* NetherFeatures::WEEPING_VINES = nullptr;
ConfiguredFeature* NetherFeatures::CRIMSON_ROOTS = nullptr;
ConfiguredFeature* NetherFeatures::BASALT_PILLAR = nullptr;
ConfiguredFeature* NetherFeatures::SPRING_LAVA_NETHER = nullptr;
ConfiguredFeature* NetherFeatures::SPRING_NETHER_CLOSED = nullptr;
ConfiguredFeature* NetherFeatures::SPRING_NETHER_OPEN = nullptr;
ConfiguredFeature* NetherFeatures::FIRE = nullptr;
ConfiguredFeature* NetherFeatures::SOUL_FIRE = nullptr;

namespace {

DeltaFeature s_deltaFeature;
SteppedColumnClusterFeature s_steppedColumnClusterFeature;
ReplaceBlobsFeature s_replaceBlobsFeature;
RandomNeighborSpreadFeature s_randomNeighborSpreadFeature;
SimpleBlockFeature s_simpleBlockFeature;
BlockColumnFeature s_blockColumnFeature;
OverlayFeature s_overlayFeature;
WeightedRandomSelectorFeature s_weightedSelectorFeature;
SingleBlockPillarFeature s_singleBlockPillarFeature;
ProjectedRandomPatchySquareFeature s_patchySquareFeature;
SpringFeature s_springFeature;

std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
std::deque<PlacedFeature> s_placed;
std::vector<std::shared_ptr<BlockStateProvider>> s_providers;
std::vector<std::unique_ptr<SimpleBlockConfiguration>> s_simpleConfigs;
std::vector<std::unique_ptr<SteppedColumnClusterConfiguration>> s_columnClusterConfigs;
std::vector<std::unique_ptr<RandomNeighborSpreadConfiguration>> s_spreadConfigs;
std::vector<std::unique_ptr<BlockColumnConfiguration>> s_blockColumnConfigs;
std::vector<std::unique_ptr<OverlayFeatureConfiguration>> s_overlayConfigs;
std::vector<std::unique_ptr<WeightedRandomFeatureConfiguration>> s_weightedConfigs;
std::vector<std::unique_ptr<SingleBlockPillarConfiguration>> s_pillarConfigs;
std::vector<std::unique_ptr<ProjectedRandomPatchySquareConfiguration>> s_patchySquareConfigs;
std::vector<std::unique_ptr<SpringConfiguration>> s_springConfigs;
std::vector<std::unique_ptr<DeltaFeatureConfiguration>> s_deltaConfigs;
std::vector<std::unique_ptr<ReplaceSphereConfiguration>> s_sphereConfigs;

std::deque<RarityFilter> s_rarities;
std::deque<OffsetPlacement> s_offsets;
std::deque<carver::ConstantInt> s_constantInts;

BlockState* block(const char* name) {
    return ::minecraft::world::level::block::Blocks::getDefaultState(name);
}

template <typename Config, typename FeatureT>
ConfiguredFeature* configured(FeatureT* feature, std::vector<std::unique_ptr<Config>>& store,
                              std::unique_ptr<Config> config) {
    store.push_back(std::move(config));
    auto configuredFeature = std::make_unique<ConfiguredFeatureImpl<Config, FeatureT>>(feature, *store.back());
    ConfiguredFeature* raw = configuredFeature.get();
    s_features.push_back(std::move(configuredFeature));
    return raw;
}

PlacedFeature* inlinePlaced(ConfiguredFeature* feature, std::vector<PlacementModifier*> modifiers,
                            const std::string& name) {
    s_placed.emplace_back(feature, std::move(modifiers), name);
    return &s_placed.back();
}

std::shared_ptr<BlockStateProvider> simpleProvider(const char* name) {
    return std::make_shared<SimpleStateProvider>(block(name));
}

ConfiguredFeature* simpleBlock(std::shared_ptr<BlockStateProvider> provider) {
    s_providers.push_back(std::move(provider));
    return configured(&s_simpleBlockFeature, s_simpleConfigs,
                      std::make_unique<SimpleBlockConfiguration>(s_providers.back().get()));
}

carver::ConstantInt* constantInt(int value) {
    s_constantInts.emplace_back(value);
    return &s_constantInts.back();
}

// OffsetPlacement.of(x, y, z)
OffsetPlacement* offset(int x, int y, int z) {
    s_offsets.emplace_back(constantInt(x), constantInt(y), constantInt(z));
    return &s_offsets.back();
}

RarityFilter* rarity(int chance) {
    s_rarities.push_back(RarityFilter::onAverageOnceEvery(chance));
    return &s_rarities.back();
}

// SteppedColumnClusterFeature(basalt, matchesBlocks(basalt), replacedByBasaltColumns,
//   #cannot_place_basalt_pillar_on, ConstantInt(clusterReach), ConstantInt(columnCount),
//   columnReach, height)
PlacedFeature* basaltColumnCluster(int clusterReach, int columnCount,
                                   std::shared_ptr<carver::IntProvider> columnReach,
                                   std::shared_ptr<carver::IntProvider> height,
                                   const std::shared_ptr<BlockPredicate>& replacedByBasaltColumns) {
    auto config = std::make_unique<SteppedColumnClusterConfiguration>();
    config->block = simpleProvider("minecraft:basalt");
    config->continueThrough = BlockPredicate::matchesBlocks(std::string("minecraft:basalt"));
    config->canReplace = replacedByBasaltColumns;
    config->cannotPlaceOn = "minecraft:cannot_place_basalt_pillar_on";
    config->clusterReach = std::make_shared<carver::ConstantInt>(clusterReach);
    config->columnCount = std::make_shared<carver::ConstantInt>(columnCount);
    config->columnReach = std::move(columnReach);
    config->height = std::move(height);
    return inlinePlaced(configured(&s_steppedColumnClusterFeature, s_columnClusterConfigs, std::move(config)), {},
                        "BASALT_COLUMN_CLUSTER_INLINE");
}

// NetherFeatures.createVines: BlockColumnFeature([WeightedListInt{
// UniformInt(min-1, max-1) 10, UniformInt(min, 2max-1) 2, ConstantInt(min-1) 3}
// x main, ConstantInt(1) x tip age UniformInt(17, 25)], direction, ONLY_IN_AIR,
// prioritizeTip)
ConfiguredFeature* createVines(int minHeight, int maxHeight, core::Direction direction,
                               const char* mainBlock, const char* tipBlock) {
    auto mainHeight = std::make_shared<util::WeightedListInt>(std::vector<util::WeightedIntEntry>{
        {std::make_shared<util::UniformInt>(minHeight - 1, maxHeight - 1), 10},
        {std::make_shared<util::UniformInt>(minHeight, maxHeight * 2 - 1), 2},
        {std::make_shared<util::ConstantInt>(minHeight - 1), 3}});
    auto tip = std::make_shared<RandomizedIntStateProvider>(simpleProvider(tipBlock), "age", 17, 25);
    auto column = std::make_unique<BlockColumnConfiguration>(
        std::vector<BlockColumnConfiguration::Layer>{
            BlockColumnConfiguration::layer(mainHeight, simpleProvider(mainBlock)),
            BlockColumnConfiguration::layer(std::make_shared<util::ConstantInt>(1), tip)},
        direction, BlockPredicate::ONLY_IN_AIR_PREDICATE, true);
    return configured(&s_blockColumnFeature, s_blockColumnConfigs, std::move(column));
}

// SingleBlockPillarFeature(basalt, ONLY_IN_AIR, DOWN, chance[, cap])
ConfiguredFeature* basaltPillar(float chanceToContinue, PlacedFeature* cap) {
    auto config = std::make_unique<SingleBlockPillarConfiguration>();
    config->block = simpleProvider("minecraft:basalt");
    config->canReplace = BlockPredicate::ONLY_IN_AIR_PREDICATE;
    config->direction = core::Direction::DOWN;
    config->chanceToContinue = chanceToContinue;
    config->capFeature = cap;
    return configured(&s_singleBlockPillarFeature, s_pillarConfigs, std::move(config));
}

} // namespace

void NetherFeatures::bootstrap() {
    if (s_initialized) return;

    // DELTA - DeltaFeature(lava, magma_block, UniformInt(3, 7), UniformInt(0, 2))
    DELTA = configured(&s_deltaFeature, s_deltaConfigs, std::make_unique<DeltaFeatureConfiguration>(
        block("minecraft:lava"), block("minecraft:magma_block"),
        std::make_shared<util::UniformInt>(3, 7), std::make_shared<util::UniformInt>(0, 2)));

    // SMALL_BASALT_COLUMNS / LARGE_BASALT_COLUMNS - WeightedRandomSelector
    // {cluster(5, 50) 9, cluster(8, 15) 1}; replacedByBasaltColumns =
    // anyOf(ONLY_IN_AIR, allOf(matchesBlocks(lava), heightRange(bottom, seaLevel)))
    {
        auto replacedByBasaltColumns = BlockPredicate::anyOf(
            BlockPredicate::ONLY_IN_AIR_PREDICATE,
            BlockPredicate::allOf(
                BlockPredicate::matchesBlocks(std::string("minecraft:lava")),
                BlockPredicate::heightRange(BlockPredicate::HeightAnchor::bottom(),
                                            BlockPredicate::HeightAnchor::seaLevel())));
        auto columns = [&](std::function<std::shared_ptr<carver::IntProvider>()> columnReach,
                           std::function<std::shared_ptr<carver::IntProvider>()> height) {
            return configured(&s_weightedSelectorFeature, s_weightedConfigs,
                std::make_unique<WeightedRandomFeatureConfiguration>(
                    std::vector<WeightedRandomFeatureConfiguration::Entry>{
                        {basaltColumnCluster(5, 50, columnReach(), height(), replacedByBasaltColumns), 9},
                        {basaltColumnCluster(8, 15, columnReach(), height(), replacedByBasaltColumns), 1}}));
        };
        SMALL_BASALT_COLUMNS = columns(
            [] { return std::make_shared<carver::ConstantInt>(1); },
            [] { return std::make_shared<carver::UniformInt>(1, 4); });
        LARGE_BASALT_COLUMNS = columns(
            [] { return std::make_shared<carver::UniformInt>(2, 3); },
            [] { return std::make_shared<carver::UniformInt>(5, 10); });
    }

    // BASALT_BLOBS / BLACKSTONE_BLOBS - ReplaceBlobsFeature(netherrack, x, UniformInt(3, 7))
    BASALT_BLOBS = configured(&s_replaceBlobsFeature, s_sphereConfigs, std::make_unique<ReplaceSphereConfiguration>(
        block("minecraft:netherrack"), block("minecraft:basalt"), std::make_shared<util::UniformInt>(3, 7)));
    BLACKSTONE_BLOBS = configured(&s_replaceBlobsFeature, s_sphereConfigs, std::make_unique<ReplaceSphereConfiguration>(
        block("minecraft:netherrack"), block("minecraft:blackstone"), std::make_shared<util::UniformInt>(3, 7)));

    // GLOWSTONE_EXTRA - RandomNeighborSpreadFeature(glowstone, [glowstone],
    // ONLY_IN_AIR, ConstantInt(1500), TrapezoidInt.triangle(7), UniformInt(-11, 0))
    {
        auto config = std::make_unique<RandomNeighborSpreadConfiguration>();
        config->block = simpleProvider("minecraft:glowstone");
        config->acceptedNeighbors = {"minecraft:glowstone"};
        config->canReplace = BlockPredicate::ONLY_IN_AIR_PREDICATE;
        config->attempts = std::make_shared<carver::ConstantInt>(1500);
        config->xzOffset = std::make_shared<carver::TrapezoidInt>(carver::TrapezoidInt::triangle(7));
        config->yOffset = std::make_shared<carver::UniformInt>(-11, 0);
        GLOWSTONE_EXTRA = configured(&s_randomNeighborSpreadFeature, s_spreadConfigs, std::move(config));
    }

    // Forest vegetation - SimpleBlockFeature over weighted states (the
    // placement does the spreading since 26.3)
    CRIMSON_FOREST_VEGETATION = simpleBlock(std::make_shared<WeightedStateProvider>(
        std::vector<WeightedStateEntry>{
            {block("minecraft:crimson_roots"), 87},
            {block("minecraft:crimson_fungus"), 11},
            {block("minecraft:warped_fungus"), 1}}));
    WARPED_FOREST_VEGETATION = simpleBlock(std::make_shared<WeightedStateProvider>(
        std::vector<WeightedStateEntry>{
            {block("minecraft:warped_roots"), 85},
            {block("minecraft:crimson_roots"), 1},
            {block("minecraft:warped_fungus"), 13},
            {block("minecraft:crimson_fungus"), 1}}));
    NETHER_SPROUTS = simpleBlock(simpleProvider("minecraft:nether_sprouts"));

    // TWISTING_VINES - createVines(1, 8, UP, twisting_vines_plant, twisting_vines)
    TWISTING_VINES = createVines(1, 8, core::Direction::UP, "minecraft:twisting_vines_plant",
                                 "minecraft:twisting_vines");

    // WEEPING_VINES - Overlay[ RandomNeighborSpread(nether_wart_block,
    // [netherrack, nether_wart_block], ONLY_IN_AIR, 200, triangle(5),
    // TrapezoidInt(-4, 1, 3)), createVines(2, 9, DOWN, ...) placed with
    // Count(100), Offset(triangle(7), TrapezoidInt(-4, 1, 3)),
    // Filter(ONLY_IN_AIR && above in [netherrack, nether_wart_block]) ]
    {
        auto roof = std::make_unique<RandomNeighborSpreadConfiguration>();
        roof->block = simpleProvider("minecraft:nether_wart_block");
        roof->acceptedNeighbors = {"minecraft:netherrack", "minecraft:nether_wart_block"};
        roof->canReplace = BlockPredicate::ONLY_IN_AIR_PREDICATE;
        roof->attempts = std::make_shared<carver::ConstantInt>(200);
        roof->xzOffset = std::make_shared<carver::TrapezoidInt>(carver::TrapezoidInt::triangle(5));
        roof->yOffset = std::make_shared<carver::TrapezoidInt>(-4, 1, 3);
        PlacedFeature* roofPlaced = inlinePlaced(
            configured(&s_randomNeighborSpreadFeature, s_spreadConfigs, std::move(roof)), {},
            "WEEPING_VINES_ROOF_INLINE");

        static carver::ConstantInt s_vineCount(100);
        static CountPlacement s_vineCountPlacement = CountPlacement::of(&s_vineCount);
        static carver::TrapezoidInt s_vineXz = carver::TrapezoidInt::triangle(7);
        static carver::TrapezoidInt s_vineY(-4, 1, 3);
        static OffsetPlacement s_vineOffset(&s_vineXz, &s_vineY, &s_vineXz);
        static BlockPredicateFilter s_vineFilter = BlockPredicateFilter::forPredicate(BlockPredicate::allOf(
            BlockPredicate::ONLY_IN_AIR_PREDICATE,
            BlockPredicate::matchesBlocks(core::Vec3i(0, 1, 0),
                std::vector<std::string>{"minecraft:netherrack", "minecraft:nether_wart_block"})));
        PlacedFeature* vinesPlaced = inlinePlaced(
            createVines(2, 9, core::Direction::DOWN, "minecraft:weeping_vines_plant", "minecraft:weeping_vines"),
            {&s_vineCountPlacement, &s_vineOffset, &s_vineFilter}, "WEEPING_VINES_COLUMNS_INLINE");

        WEEPING_VINES = configured(&s_overlayFeature, s_overlayConfigs,
            std::make_unique<OverlayFeatureConfiguration>(std::vector<PlacedFeature*>{roofPlaced, vinesPlaced}));
    }

    // CRIMSON_ROOTS - SimpleBlockFeature(crimson_roots)
    CRIMSON_ROOTS = simpleBlock(simpleProvider("minecraft:crimson_roots"));

    // BASALT_PILLAR - Overlay[ SingleBlockPillar(basalt, ONLY_IN_AIR, DOWN,
    // 1.0, cap = Overlay[ ProjectedRandomPatchySquare(basalt where the block
    // below is not air, ONLY_IN_AIR, ConstantInt(3), 3) one block down,
    // SimpleBlock(basalt) at +x, -x, +z, -z each RarityFilter(2) ]),
    // SingleBlockPillar(basalt, ONLY_IN_AIR, DOWN, 0.9) at +x, -x, +z, -z ]
    {
        auto patchProvider = std::make_shared<RuleBasedBlockStateProvider>(
            nullptr,
            std::vector<RuleBasedBlockStateProvider::Rule>{
                {BlockPredicate::not_(BlockPredicate::matchesTag(core::Vec3i(0, -1, 0), "minecraft:air")),
                 simpleProvider("minecraft:basalt")}});
        auto square = std::make_unique<ProjectedRandomPatchySquareConfiguration>();
        square->block = patchProvider;
        square->projectThrough = BlockPredicate::ONLY_IN_AIR_PREDICATE;
        square->size = std::make_shared<carver::ConstantInt>(3);
        square->maxProjectionHeight = 3;
        PlacedFeature* squarePlaced = inlinePlaced(
            configured(&s_patchySquareFeature, s_patchySquareConfigs, std::move(square)),
            {offset(0, -1, 0)}, "BASALT_PILLAR_BASE_INLINE");

        ConfiguredFeature* basaltBlock = simpleBlock(simpleProvider("minecraft:basalt"));
        auto sideBlock = [&](int x, int z) {
            return inlinePlaced(basaltBlock, {rarity(2), offset(x, 0, z)}, "BASALT_PILLAR_SIDE_INLINE");
        };
        ConfiguredFeature* cap = configured(&s_overlayFeature, s_overlayConfigs,
            std::make_unique<OverlayFeatureConfiguration>(std::vector<PlacedFeature*>{
                squarePlaced, sideBlock(1, 0), sideBlock(-1, 0), sideBlock(0, 1), sideBlock(0, -1)}));
        PlacedFeature* capPlaced = inlinePlaced(cap, {}, "BASALT_PILLAR_CAP_INLINE");

        PlacedFeature* mainPillar = inlinePlaced(basaltPillar(1.0f, capPlaced), {}, "BASALT_PILLAR_MAIN_INLINE");
        auto sidePillar = [&](int x, int z) {
            return inlinePlaced(basaltPillar(0.9f, nullptr), {offset(x, 0, z)}, "BASALT_PILLAR_SIDE_PILLAR_INLINE");
        };
        BASALT_PILLAR = configured(&s_overlayFeature, s_overlayConfigs,
            std::make_unique<OverlayFeatureConfiguration>(std::vector<PlacedFeature*>{
                mainPillar, sidePillar(1, 0), sidePillar(-1, 0), sidePillar(0, 1), sidePillar(0, -1)}));
    }

    // Springs - SpringFeature(lava, requiresBlockBelow, rockCount, holeCount, validBlocks)
    auto makeSpring = [&](bool requiresBlockBelow, int rockCount, int holeCount,
                          const std::set<std::string>& validBlocks) -> ConfiguredFeature* {
        return configured(&s_springFeature, s_springConfigs, std::make_unique<SpringConfiguration>(
            "minecraft:lava", requiresBlockBelow, rockCount, holeCount, validBlocks));
    };
    SPRING_LAVA_NETHER = makeSpring(true, 4, 1,
        {"minecraft:netherrack", "minecraft:soul_sand", "minecraft:gravel",
         "minecraft:magma_block", "minecraft:blackstone"});
    SPRING_NETHER_CLOSED = makeSpring(false, 5, 0, {"minecraft:netherrack"});
    SPRING_NETHER_OPEN = makeSpring(false, 4, 1, {"minecraft:netherrack"});

    // FIRE / SOUL_FIRE - SimpleBlockFeature (patch spreading is in the placement)
    FIRE = simpleBlock(simpleProvider("minecraft:fire"));
    SOUL_FIRE = simpleBlock(simpleProvider("minecraft:soul_fire"));

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
