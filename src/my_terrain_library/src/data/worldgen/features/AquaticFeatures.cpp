#include "data/worldgen/features/AquaticFeatures.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "util/IntProvider.h"
#include <deque>
#include <memory>
#include <string>
#include <vector>

// Reference: 26.3 net/minecraft/data/worldgen/features/AquaticFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;
using namespace levelgen;
using namespace levelgen::placement;
using levelgen::blockpredicates::BlockPredicate;
using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::RandomBlockProvider;
using levelgen::feature::stateproviders::RandomizedIntStateProvider;
using levelgen::feature::stateproviders::RotatedBlockProvider;
using Blocks = ::minecraft::world::level::block::Blocks;

bool AquaticFeatures::s_initialized = false;

// ConfiguredFeature pointers
ConfiguredFeature* AquaticFeatures::SEAGRASS_SHORT = nullptr;
ConfiguredFeature* AquaticFeatures::SEAGRASS_SLIGHTLY_LESS_SHORT = nullptr;
ConfiguredFeature* AquaticFeatures::SEAGRASS_MID = nullptr;
ConfiguredFeature* AquaticFeatures::SEAGRASS_TALL = nullptr;
ConfiguredFeature* AquaticFeatures::SEA_PICKLE = nullptr;
ConfiguredFeature* AquaticFeatures::KELP = nullptr;
ConfiguredFeature* AquaticFeatures::WARM_OCEAN_VEGETATION = nullptr;

namespace {

SimpleBlockFeature s_simpleBlockFeature;
WeightedRandomSelectorFeature s_weightedSelectorFeature;
SimpleRandomSelectorFeature s_simpleRandomSelectorFeature;
OverlayFeature s_overlayFeature;
NoOpFeature s_noOpFeature;
BlockColumnFeature s_blockColumnFeature;
CoralTreeFeature s_coralTreeFeature;
CoralClawFeature s_coralClawFeature;

std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
std::deque<PlacedFeature> s_placed;
std::vector<std::shared_ptr<BlockStateProvider>> s_providers;
std::vector<std::unique_ptr<SimpleBlockConfiguration>> s_simpleConfigs;
std::vector<std::unique_ptr<WeightedRandomFeatureConfiguration>> s_weightedConfigs;
std::vector<std::unique_ptr<SimpleRandomFeatureConfiguration>> s_simpleRandomConfigs;
std::vector<std::unique_ptr<OverlayFeatureConfiguration>> s_overlayConfigs;
std::vector<std::unique_ptr<CoralShapeConfiguration>> s_coralConfigs;
std::vector<std::unique_ptr<BlockColumnConfiguration>> s_columnConfigs;
NoneFeatureConfiguration s_noneConfig;

std::deque<BlockPredicateFilter> s_filters;
std::deque<RandomChancePlacement> s_chances;
std::deque<OffsetPlacement> s_offsets;
std::deque<CuboidPlacement> s_cuboids;
std::deque<carver::ConstantInt> s_constantInts;
std::deque<carver::UniformInt> s_uniformInts;

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

BlockPredicateFilter* filter(std::shared_ptr<BlockPredicate> predicate) {
    s_filters.push_back(BlockPredicateFilter::forPredicate(std::move(predicate)));
    return &s_filters.back();
}

std::shared_ptr<BlockPredicate> matchesWater(const core::Vec3i& offset = core::Vec3i(0, 0, 0)) {
    return BlockPredicate::matchesBlocks(offset, std::vector<std::string>{"minecraft:water"});
}

// AquaticFeatures.seagrass(tallPercentage)
ConfiguredFeature* seagrass(int tallPercentage) {
    PlacedFeature* tall = inlinePlaced(simpleBlock(BlockStateProvider::simple(Blocks::TALL_SEAGRASS)),
                                       {filter(matchesWater(core::Vec3i(0, 1, 0)))}, "SEAGRASS_TALL_INLINE");
    PlacedFeature* shortGrass = inlinePlaced(simpleBlock(BlockStateProvider::simple(Blocks::SEAGRASS)), {},
                                             "SEAGRASS_SHORT_INLINE");
    return configured(&s_weightedSelectorFeature, s_weightedConfigs,
                      std::make_unique<WeightedRandomFeatureConfiguration>(
                          std::vector<WeightedRandomFeatureConfiguration::Entry>{
                              {tall, tallPercentage}, {shortGrass, 100 - tallPercentage}}));
}

// AquaticFeatures.wallCoral(direction)
PlacedFeature* wallCoral(core::Direction direction) {
    auto provider = std::make_shared<RotatedBlockProvider>(
        std::make_shared<RandomBlockProvider>("minecraft:wall_corals"), direction);
    s_chances.emplace_back(0.2f);
    RandomChancePlacement* chance = &s_chances.back();
    return inlinePlaced(simpleBlock(provider),
                        {chance, offset(core::getStepX(direction), core::getStepY(direction), core::getStepZ(direction)),
                         filter(matchesWater())},
                        "WALL_CORAL_INLINE");
}

} // namespace

void AquaticFeatures::bootstrap() {
    if (s_initialized) return;

    // SEAGRASS_* - WeightedRandomSelector{tall seagrass (water above) t,
    // seagrass 100 - t}
    SEAGRASS_SHORT = seagrass(30);
    SEAGRASS_SLIGHTLY_LESS_SHORT = seagrass(40);
    SEAGRASS_MID = seagrass(60);
    SEAGRASS_TALL = seagrass(80);

    // SEA_PICKLE - SimpleBlockFeature(RandomizedIntStateProvider(sea_pickle,
    // PICKLES, UniformInt(1, 4)))
    SEA_PICKLE = simpleBlock(std::make_shared<RandomizedIntStateProvider>(
        BlockStateProvider::simple(Blocks::getDefaultState("minecraft:sea_pickle")), "pickles", 1, 4));

    // KELP - BlockColumnFeature([UniformInt(0, 9) kelp_plant,
    // ConstantInt(1) kelp age UniformInt(20, 23)], UP,
    // allOf(water, water above), prioritizeTip)
    {
        auto kelpTip = std::make_shared<RandomizedIntStateProvider>(
            BlockStateProvider::simple(Blocks::getDefaultState("minecraft:kelp")), "age", 20, 23);
        s_providers.push_back(kelpTip);
        auto kelpPlant = BlockStateProvider::simple(Blocks::getDefaultState("minecraft:kelp_plant"));
        s_providers.push_back(kelpPlant);
        auto column = std::make_unique<BlockColumnConfiguration>(
            std::vector<BlockColumnConfiguration::Layer>{
                BlockColumnConfiguration::layer(std::make_shared<util::UniformInt>(0, 9), kelpPlant),
                BlockColumnConfiguration::layer(std::make_shared<util::ConstantInt>(1), kelpTip)},
            core::Direction::UP,
            BlockPredicate::allOf(matchesWater(), matchesWater(core::Vec3i(0, 1, 0))),
            true);
        KELP = configured(&s_blockColumnFeature, s_columnConfigs, std::move(column));
    }

    // coral/block_decoration - Overlay[ WeightedRandomSelector{coral 20,
    // SEA_PICKLE 3, NoOp 57} one block up, wall corals N, E, S, W ]
    ConfiguredFeature* coralDecoration = nullptr;
    {
        PlacedFeature* coralPlant = inlinePlaced(
            simpleBlock(std::make_shared<RandomBlockProvider>("minecraft:corals")), {}, "CORAL_PLANT_INLINE");
        PlacedFeature* pickle = inlinePlaced(SEA_PICKLE, {}, "SEA_PICKLE_INLINE");
        s_features.push_back(std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, NoOpFeature>>(
            &s_noOpFeature, s_noneConfig));
        PlacedFeature* nothing = inlinePlaced(s_features.back().get(), {}, "NO_OP_INLINE");
        ConfiguredFeature* top = configured(&s_weightedSelectorFeature, s_weightedConfigs,
            std::make_unique<WeightedRandomFeatureConfiguration>(
                std::vector<WeightedRandomFeatureConfiguration::Entry>{{coralPlant, 20}, {pickle, 3}, {nothing, 57}}));
        coralDecoration = configured(&s_overlayFeature, s_overlayConfigs,
            std::make_unique<OverlayFeatureConfiguration>(std::vector<PlacedFeature*>{
                inlinePlaced(top, {offset(0, 1, 0)}, "CORAL_TOP_INLINE"),
                wallCoral(core::Direction::NORTH),
                wallCoral(core::Direction::EAST),
                wallCoral(core::Direction::SOUTH),
                wallCoral(core::Direction::WEST)}));
    }
    PlacedFeature* coralDecorationPlaced = inlinePlaced(coralDecoration, {}, "CORAL_BLOCK_DECORATION_INLINE");

    // coral/<type>_block = Overlay[SimpleBlock(coral block), block_decoration];
    // WARM_OCEAN_VEGETATION = SimpleRandomSelector over, per block type (tube,
    // brain, bubble, fire, horn): coral tree, coral claw, coral mushroom
    // (the block feature with Offset.vertical(UniformInt(-3, -1)),
    // Cuboid(UniformInt(3, 5), UniformInt(3, 5), no edges, no interior),
    // RandomChance(0.9), coral-allowed).
    {
        auto coralAllowedPredicate = BlockPredicate::allOf(
            BlockPredicate::anyOf(matchesWater(), BlockPredicate::matchesTag("minecraft:corals")),
            matchesWater(core::Vec3i(0, 1, 0)));
        std::vector<PlacedFeature*> variants;
        for (const char* type : {"tube", "brain", "bubble", "fire", "horn"}) {
            ConfiguredFeature* coralBlock = configured(&s_overlayFeature, s_overlayConfigs,
                std::make_unique<OverlayFeatureConfiguration>(std::vector<PlacedFeature*>{
                    inlinePlaced(simpleBlock(BlockStateProvider::simple(
                                     Blocks::getDefaultState("minecraft:" + std::string(type) + "_coral_block"))),
                                 {}, "CORAL_BLOCK_INLINE"),
                    coralDecorationPlaced}));
            PlacedFeature* allowedBlock = inlinePlaced(coralBlock, {filter(coralAllowedPredicate)}, "CORAL_ALLOWED_INLINE");

            ConfiguredFeature* tree = configured(&s_coralTreeFeature, s_coralConfigs,
                                                 std::make_unique<CoralShapeConfiguration>(allowedBlock));
            ConfiguredFeature* claw = configured(&s_coralClawFeature, s_coralConfigs,
                                                 std::make_unique<CoralShapeConfiguration>(allowedBlock));
            variants.push_back(inlinePlaced(tree, {}, "CORAL_TREE_INLINE"));
            variants.push_back(inlinePlaced(claw, {}, "CORAL_CLAW_INLINE"));

            s_uniformInts.emplace_back(-3, -1);
            carver::UniformInt* sink = &s_uniformInts.back();
            s_offsets.emplace_back(constantInt(0), sink, constantInt(0));
            OffsetPlacement* down = &s_offsets.back();
            s_uniformInts.emplace_back(3, 5);
            carver::UniformInt* size = &s_uniformInts.back();
            s_cuboids.emplace_back(size, size, false, false);
            CuboidPlacement* cuboid = &s_cuboids.back();
            s_chances.emplace_back(0.9f);
            RandomChancePlacement* keep = &s_chances.back();
            variants.push_back(inlinePlaced(coralBlock, {down, cuboid, keep, filter(coralAllowedPredicate)},
                                            "CORAL_MUSHROOM_INLINE"));
        }
        WARM_OCEAN_VEGETATION = configured(&s_simpleRandomSelectorFeature, s_simpleRandomConfigs,
                                           std::make_unique<SimpleRandomFeatureConfiguration>(variants));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
