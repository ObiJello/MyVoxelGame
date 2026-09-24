#include "data/worldgen/features/MiscOverworldFeatures.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "util/IntProvider.h"
#include "levelgen/feature/TemplateFeature.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include <deque>

// Reference: net/minecraft/data/worldgen/features/MiscOverworldFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;
using namespace levelgen;
using namespace levelgen::feature::stateproviders;
using Blocks = ::minecraft::world::level::block::Blocks;

// Static members - Feature instances
DiskFeature MiscOverworldFeatures::s_diskFeature;
SnowAndFreezeFeature MiscOverworldFeatures::s_snowAndFreezeFeature;
IceSpikeFeature MiscOverworldFeatures::s_iceSpikeFeature;
ForestRockFeature MiscOverworldFeatures::s_forestRockFeature;
IcebergFeature MiscOverworldFeatures::s_icebergFeature;
BlueIceFeature MiscOverworldFeatures::s_blueIceFeature;
LakeFeature MiscOverworldFeatures::s_lakeFeature;
SpringFeature MiscOverworldFeatures::s_springFeature;
VoidStartPlatformFeature MiscOverworldFeatures::s_voidStartPlatformFeature;
BonusChestFeature MiscOverworldFeatures::s_bonusChestFeature;
bool MiscOverworldFeatures::s_initialized = false;

// ConfiguredFeature pointers - Ice features
ConfiguredFeature* MiscOverworldFeatures::ICE_SPIKE = nullptr;
ConfiguredFeature* MiscOverworldFeatures::ICE_PATCH = nullptr;
ConfiguredFeature* MiscOverworldFeatures::ICEBERG_PACKED = nullptr;
ConfiguredFeature* MiscOverworldFeatures::ICEBERG_BLUE = nullptr;
ConfiguredFeature* MiscOverworldFeatures::BLUE_ICE = nullptr;

// ConfiguredFeature pointers - Misc features
ConfiguredFeature* MiscOverworldFeatures::FOREST_ROCK = nullptr;
ConfiguredFeature* MiscOverworldFeatures::LAKE_LAVA = nullptr;
ConfiguredFeature* MiscOverworldFeatures::SULFUR_SPRING = nullptr;
ConfiguredFeature* MiscOverworldFeatures::SULFUR_POOL = nullptr;

// ConfiguredFeature pointers - Disk features
ConfiguredFeature* MiscOverworldFeatures::DISK_CLAY = nullptr;
ConfiguredFeature* MiscOverworldFeatures::DISK_GRAVEL = nullptr;
ConfiguredFeature* MiscOverworldFeatures::DISK_SAND = nullptr;
ConfiguredFeature* MiscOverworldFeatures::DISK_GRASS = nullptr;

// ConfiguredFeature pointers - Special features
ConfiguredFeature* MiscOverworldFeatures::FREEZE_TOP_LAYER = nullptr;
ConfiguredFeature* MiscOverworldFeatures::BONUS_CHEST = nullptr;
ConfiguredFeature* MiscOverworldFeatures::VOID_START_PLATFORM = nullptr;
ConfiguredFeature* MiscOverworldFeatures::DESERT_WELL = nullptr;

// ConfiguredFeature pointers - Spring features
ConfiguredFeature* MiscOverworldFeatures::SPRING_LAVA_OVERWORLD = nullptr;
ConfiguredFeature* MiscOverworldFeatures::SPRING_LAVA_FROZEN = nullptr;
ConfiguredFeature* MiscOverworldFeatures::SPRING_WATER = nullptr;

// Storage for ConfiguredFeature instances
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<DiskConfiguration>> s_diskConfigs;
static std::vector<std::unique_ptr<NoneFeatureConfiguration>> s_noneConfigs;
static std::vector<std::unique_ptr<BlockStateConfiguration>> s_blockStateConfigs;
static std::vector<std::unique_ptr<LakeConfiguration>> s_lakeConfigs;
static std::vector<std::unique_ptr<SpringConfiguration>> s_springConfigs;
static std::vector<std::shared_ptr<RuleBasedBlockStateProvider>> s_stateProviders;

void MiscOverworldFeatures::bootstrap() {
    if (s_initialized) return;

    // =========================================================================
    // ICE_SPIKE
    // Reference: MiscOverworldFeatures.java line 43
    // Feature.ICE_SPIKE with NoneFeatureConfiguration
    // =========================================================================
    {
        auto config = std::make_unique<NoneFeatureConfiguration>();
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, IceSpikeFeature>>(
            &s_iceSpikeFeature, *config);
        ICE_SPIKE = feature.get();
        s_noneConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // ICE_PATCH
    // Reference: MiscOverworldFeatures.java line 44
    // DiskConfiguration(RuleBasedBlockStateProvider.simple(PACKED_ICE),
    //   BlockPredicate.matchesBlocks(DIRT, GRASS_BLOCK, PODZOL, COARSE_DIRT, MYCELIUM, SNOW_BLOCK, ICE),
    //   UniformInt.of(2, 3), 1)
    // =========================================================================
    {
        auto packedIceProvider = std::make_shared<SimpleStateProvider>("minecraft:packed_ice");
        auto stateProvider = std::make_shared<RuleBasedBlockStateProvider>(packedIceProvider);
        s_stateProviders.push_back(stateProvider);

        auto targetPredicate = blockpredicates::BlockPredicate::matchesBlocks(
            std::vector<std::string>{
                "minecraft:dirt", "minecraft:grass_block", "minecraft:podzol",
                "minecraft:coarse_dirt", "minecraft:mycelium", "minecraft:snow_block", "minecraft:ice"
            }
        );

        auto config = std::make_unique<DiskConfiguration>(
            stateProvider,
            targetPredicate,
            std::make_shared<util::UniformInt>(2, 3),  // radius
            1  // halfHeight
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<DiskConfiguration, DiskFeature>>(
            &s_diskFeature, *config);
        ICE_PATCH = feature.get();
        s_diskConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // FOREST_ROCK
    // Reference: MiscOverworldFeatures.java line 45
    // Feature.FOREST_ROCK, BlockStateConfiguration(Blocks.MOSSY_COBBLESTONE.defaultBlockState())
    // =========================================================================
    {
        auto config = std::make_unique<BlockStateConfiguration>(
            Blocks::MOSSY_COBBLESTONE->defaultBlockState()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, ForestRockFeature>>(
            &s_forestRockFeature, *config);
        FOREST_ROCK = feature.get();
        s_blockStateConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // ICEBERG_PACKED
    // Reference: MiscOverworldFeatures.java line 46
    // Feature.ICEBERG, BlockStateConfiguration(Blocks.PACKED_ICE.defaultBlockState())
    // =========================================================================
    {
        auto config = std::make_unique<BlockStateConfiguration>(
            Blocks::PACKED_ICE->defaultBlockState()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, IcebergFeature>>(
            &s_icebergFeature, *config);
        ICEBERG_PACKED = feature.get();
        s_blockStateConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // ICEBERG_BLUE
    // Reference: MiscOverworldFeatures.java line 47
    // Feature.ICEBERG, BlockStateConfiguration(Blocks.BLUE_ICE.defaultBlockState())
    // =========================================================================
    {
        auto config = std::make_unique<BlockStateConfiguration>(
            Blocks::BLUE_ICE->defaultBlockState()
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, IcebergFeature>>(
            &s_icebergFeature, *config);
        ICEBERG_BLUE = feature.get();
        s_blockStateConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // BLUE_ICE
    // Reference: MiscOverworldFeatures.java line 48
    // Feature.BLUE_ICE with NoneFeatureConfiguration
    // =========================================================================
    {
        auto config = std::make_unique<NoneFeatureConfiguration>();
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, BlueIceFeature>>(
            &s_blueIceFeature, *config);
        BLUE_ICE = feature.get();
        s_noneConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // LAKE_LAVA
    // Reference: MiscOverworldFeatures.java line 49
    // Feature.LAKE, LakeFeature.Configuration(
    //   BlockStateProvider.simple(Blocks.LAVA.defaultBlockState()),
    //   BlockStateProvider.simple(Blocks.STONE.defaultBlockState()))
    // =========================================================================
    {
        auto fluidProvider = std::make_shared<SimpleStateProvider>("minecraft:lava");
        auto barrierProvider = std::make_shared<SimpleStateProvider>("minecraft:stone");

        auto config = std::make_unique<LakeConfiguration>(fluidProvider, barrierProvider);
        auto feature = std::make_unique<ConfiguredFeatureImpl<LakeConfiguration, LakeFeature>>(
            &s_lakeFeature, *config);
        LAKE_LAVA = feature.get();
        s_lakeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SULFUR_POOL (26.3 MiscOverworldFeatures.java): a SequenceFeature of
    //   LakeFeature(water, sulfur, not(matchesBlocks(sulfur_spike)),
    //               not(#features_cannot_replace), not(#lava_pool_stone_cannot_replace))
    //   SimpleBlockFeature(potent_sulfur[wet]) at
    //       EnvironmentScan(DOWN, allOf(solid, matchesFluids(UP, water)), 4)
    // SULFUR_SPRING: WeightedRandomSelector over sequences of a tuff cover
    // and a sulfur spring template 7 blocks down (small 200, medium 90,
    // large 20, extra large 5).
    // =========================================================================
    {
        using namespace levelgen::placement;
        using levelgen::blockpredicates::BlockPredicate;
        static SequenceFeature s_sequenceFeature;
        static WeightedRandomSelectorFeature s_weightedSelectorFeature;
        static SimpleBlockFeature s_simpleBlockFeature;
        static TemplateFeature s_templateFeature;
        static std::deque<PlacedFeature> s_inlinePlaced;
        static std::deque<CountPlacement> s_counts;
        static std::deque<levelgen::carver::ConstantInt> s_constantInts;
        static std::deque<levelgen::carver::TrapezoidInt> s_trapezoidInts;
        static std::deque<RandomOffsetPlacement> s_offsets;
        static std::deque<EnvironmentScanPlacement> s_scans;
        static std::deque<BlockPredicateFilter> s_filters;
        static std::vector<std::unique_ptr<SimpleBlockConfiguration>> s_simpleConfigs;
        static std::vector<std::unique_ptr<SequenceFeatureConfiguration>> s_sequenceConfigs;
        static std::vector<std::unique_ptr<WeightedRandomFeatureConfiguration>> s_weightedConfigs;
        static std::vector<std::unique_ptr<TemplateFeatureConfiguration>> s_templateConfigs;
        static std::vector<std::shared_ptr<BlockStateProvider>> s_providers;

        auto inlinePlaced = [](ConfiguredFeature* feature, std::vector<PlacementModifier*> modifiers,
                               const std::string& name) -> PlacedFeature* {
            s_inlinePlaced.emplace_back(feature, modifiers, name);
            return &s_inlinePlaced.back();
        };
        auto simpleBlock = [&](BlockState* state) -> ConfiguredFeature* {
            s_providers.push_back(std::make_shared<SimpleStateProvider>(state));
            s_simpleConfigs.push_back(std::make_unique<SimpleBlockConfiguration>(s_providers.back().get()));
            auto feature = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
                &s_simpleBlockFeature, *s_simpleConfigs.back());
            ConfiguredFeature* raw = feature.get();
            s_features.push_back(std::move(feature));
            return raw;
        };
        auto sequence = [&](std::vector<PlacedFeature*> list) -> ConfiguredFeature* {
            s_sequenceConfigs.push_back(std::make_unique<SequenceFeatureConfiguration>(std::move(list)));
            auto feature = std::make_unique<ConfiguredFeatureImpl<SequenceFeatureConfiguration, SequenceFeature>>(
                &s_sequenceFeature, *s_sequenceConfigs.back());
            ConfiguredFeature* raw = feature.get();
            s_features.push_back(std::move(feature));
            return raw;
        };

        // ---- SULFUR_POOL
        {
            auto lakeConfig = std::make_unique<LakeConfiguration>(
                std::make_shared<SimpleStateProvider>("minecraft:water"),
                std::make_shared<SimpleStateProvider>("minecraft:sulfur"),
                BlockPredicate::not_(BlockPredicate::matchesBlocks(std::vector<std::string>{"minecraft:sulfur_spike"})),
                BlockPredicate::not_(BlockPredicate::matchesTag("minecraft:features_cannot_replace")),
                BlockPredicate::not_(BlockPredicate::matchesTag("minecraft:lava_pool_stone_cannot_replace")));
            auto lake = std::make_unique<ConfiguredFeatureImpl<LakeConfiguration, LakeFeature>>(
                &s_lakeFeature, *lakeConfig);
            ConfiguredFeature* lakeRaw = lake.get();
            s_lakeConfigs.push_back(std::move(lakeConfig));
            s_features.push_back(std::move(lake));

            using world::level::block::state::properties::BlockStateProperties;
            using world::level::block::state::properties::PotentSulfurState;
            BlockState* wetPotentSulfur = Blocks::POTENT_SULFUR->defaultBlockState()->setValue(
                *BlockStateProperties::POTENT_SULFUR_STATE, PotentSulfurState(PotentSulfurState::WET));
            s_scans.push_back(EnvironmentScanPlacement::scanningFor(
                EnvironmentScanPlacement::Direction::DOWN,
                BlockPredicate::allOf(BlockPredicate::solid(),
                                      BlockPredicate::matchesFluids(core::Vec3i(0, 1, 0),
                                                                    std::vector<std::string>{"minecraft:water"})),
                4));
            SULFUR_POOL = sequence({
                inlinePlaced(lakeRaw, {}, "SULFUR_POOL_LAKE_INLINE"),
                inlinePlaced(simpleBlock(wetPotentSulfur), {&s_scans.back()}, "SULFUR_POOL_POTENT_SULFUR_INLINE"),
            });
        }

        // ---- SULFUR_SPRING
        {
            // tuffCover(count, spread): SimpleBlock(tuff) with Count(count),
            // OffsetPlacement.ofTriangle(spread, 3),
            // EnvironmentScan(DOWN, solid, 4), BlockPredicateFilter(solid)
            ConfiguredFeature* tuff = simpleBlock(Blocks::TUFF->defaultBlockState());
            auto tuffCover = [&](int count, int spread) -> PlacedFeature* {
                s_constantInts.emplace_back(count);
                s_counts.push_back(CountPlacement::of(&s_constantInts.back()));
                CountPlacement* countPlacement = &s_counts.back();
                s_trapezoidInts.push_back(levelgen::carver::TrapezoidInt::triangle(spread));
                levelgen::carver::TrapezoidInt* xz = &s_trapezoidInts.back();
                s_trapezoidInts.push_back(levelgen::carver::TrapezoidInt::triangle(3));
                levelgen::carver::TrapezoidInt* y = &s_trapezoidInts.back();
                s_offsets.push_back(RandomOffsetPlacement::of(xz, y));
                RandomOffsetPlacement* offset = &s_offsets.back();
                s_scans.push_back(EnvironmentScanPlacement::scanningFor(
                    EnvironmentScanPlacement::Direction::DOWN, BlockPredicate::solid(), 4));
                EnvironmentScanPlacement* scan = &s_scans.back();
                s_filters.push_back(BlockPredicateFilter::forPredicate(BlockPredicate::solid()));
                return inlinePlaced(tuff, {countPlacement, offset, scan, &s_filters.back()}, "SULFUR_TUFF_COVER_INLINE");
            };
            // sulfurSprings(templates): TemplateFeature with Offset.vertical(-7)
            auto springs = [&](std::vector<std::string> ids) -> PlacedFeature* {
                std::vector<TemplateFeatureConfiguration::Entry> entries;
                for (const std::string& id : ids) {
                    entries.push_back(TemplateFeatureConfiguration::Entry{id, 1, {0, 1, 2, 3}});
                }
                s_templateConfigs.push_back(std::make_unique<TemplateFeatureConfiguration>(std::move(entries)));
                auto feature = std::make_unique<ConfiguredFeatureImpl<TemplateFeatureConfiguration, TemplateFeature>>(
                    &s_templateFeature, *s_templateConfigs.back());
                ConfiguredFeature* raw = feature.get();
                s_features.push_back(std::move(feature));
                s_constantInts.emplace_back(-7);
                s_offsets.push_back(RandomOffsetPlacement::vertical(&s_constantInts.back()));
                return inlinePlaced(raw, {&s_offsets.back()}, "SULFUR_SPRING_TEMPLATE_INLINE");
            };
            auto variant = [&](int count, int spread, std::vector<std::string> ids) -> PlacedFeature* {
                return inlinePlaced(sequence({tuffCover(count, spread), springs(std::move(ids))}), {},
                                    "SULFUR_SPRING_VARIANT_INLINE");
            };
            s_weightedConfigs.push_back(std::make_unique<WeightedRandomFeatureConfiguration>(
                std::vector<WeightedRandomFeatureConfiguration::Entry>{
                    {variant(64, 7, {"minecraft:spring/sulfur_spring_small_1", "minecraft:spring/sulfur_spring_small_2",
                                     "minecraft:spring/sulfur_spring_small_3", "minecraft:spring/sulfur_spring_small_4"}), 200},
                    {variant(80, 8, {"minecraft:spring/sulfur_spring_medium_1", "minecraft:spring/sulfur_spring_medium_2",
                                     "minecraft:spring/sulfur_spring_medium_3"}), 90},
                    {variant(96, 9, {"minecraft:spring/sulfur_spring_large_1", "minecraft:spring/sulfur_spring_large_2"}), 20},
                    {variant(128, 10, {"minecraft:spring/sulfur_spring_extra_large_1"}), 5},
                }));
            auto feature = std::make_unique<ConfiguredFeatureImpl<WeightedRandomFeatureConfiguration, WeightedRandomSelectorFeature>>(
                &s_weightedSelectorFeature, *s_weightedConfigs.back());
            SULFUR_SPRING = feature.get();
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // DISK_CLAY
    // Reference: MiscOverworldFeatures.java line 50
    // DiskConfiguration(RuleBasedBlockStateProvider.simple(CLAY),
    //   BlockPredicate.matchesBlocks(DIRT, CLAY), UniformInt.of(2, 3), 1)
    // =========================================================================
    {
        auto clayProvider = std::make_shared<SimpleStateProvider>("minecraft:clay");
        auto stateProvider = std::make_shared<RuleBasedBlockStateProvider>(clayProvider);
        s_stateProviders.push_back(stateProvider);

        auto targetPredicate = blockpredicates::BlockPredicate::matchesBlocks(
            std::vector<std::string>{"minecraft:dirt", "minecraft:clay"}
        );

        auto config = std::make_unique<DiskConfiguration>(
            stateProvider,
            targetPredicate,
            std::make_shared<util::UniformInt>(2, 3),  // radius
            1  // halfHeight
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<DiskConfiguration, DiskFeature>>(
            &s_diskFeature, *config);
        DISK_CLAY = feature.get();
        s_diskConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DISK_GRAVEL
    // Reference: MiscOverworldFeatures.java line 51
    // DiskConfiguration(RuleBasedBlockStateProvider.simple(GRAVEL),
    //   BlockPredicate.matchesBlocks(DIRT, GRASS_BLOCK), UniformInt.of(2, 5), 2)
    // =========================================================================
    {
        auto gravelProvider = std::make_shared<SimpleStateProvider>("minecraft:gravel");
        auto stateProvider = std::make_shared<RuleBasedBlockStateProvider>(gravelProvider);
        s_stateProviders.push_back(stateProvider);

        auto targetPredicate = blockpredicates::BlockPredicate::matchesBlocks(
            std::vector<std::string>{"minecraft:dirt", "minecraft:grass_block"}
        );

        auto config = std::make_unique<DiskConfiguration>(
            stateProvider,
            targetPredicate,
            std::make_shared<util::UniformInt>(2, 5),  // radius
            2  // halfHeight
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<DiskConfiguration, DiskFeature>>(
            &s_diskFeature, *config);
        DISK_GRAVEL = feature.get();
        s_diskConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DISK_SAND
    // Reference: MiscOverworldFeatures.java line 52
    // DiskConfiguration with RuleBasedBlockStateProvider that places SANDSTONE below AIR
    // BlockPredicate.matchesBlocks(DIRT, GRASS_BLOCK), UniformInt.of(2, 6), 2
    // =========================================================================
    {
        auto sandProvider = std::make_shared<SimpleStateProvider>("minecraft:sand");
        auto sandstoneProvider = std::make_shared<SimpleStateProvider>("minecraft:sandstone");
        std::vector<RuleBasedBlockStateProvider::Rule> sandRules{
            {blockpredicates::BlockPredicate::matchesBlocks(
                 core::Vec3i(0, -1, 0), "minecraft:air"),
             sandstoneProvider}
        };
        auto stateProvider = std::make_shared<RuleBasedBlockStateProvider>(
            sandProvider, std::move(sandRules));
        s_stateProviders.push_back(stateProvider);

        auto targetPredicate = blockpredicates::BlockPredicate::matchesBlocks(
            std::vector<std::string>{"minecraft:dirt", "minecraft:grass_block"}
        );

        auto config = std::make_unique<DiskConfiguration>(
            stateProvider,
            targetPredicate,
            std::make_shared<util::UniformInt>(2, 6),  // radius
            2  // halfHeight
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<DiskConfiguration, DiskFeature>>(
            &s_diskFeature, *config);
        DISK_SAND = feature.get();
        s_diskConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // FREEZE_TOP_LAYER
    // Reference: MiscOverworldFeatures.java line 53
    // Feature.FREEZE_TOP_LAYER with NoneFeatureConfiguration
    // =========================================================================
    {
        auto config = std::make_unique<NoneFeatureConfiguration>();
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, SnowAndFreezeFeature>>(
            &s_snowAndFreezeFeature, *config);
        FREEZE_TOP_LAYER = feature.get();
        s_noneConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DISK_GRASS
    // Reference: MiscOverworldFeatures.java line 54
    // DiskConfiguration with RuleBasedBlockStateProvider that places GRASS_BLOCK
    // when no solid block above and no water above
    // BlockPredicate.matchesBlocks(DIRT, MUD), UniformInt.of(2, 6), 2
    // =========================================================================
    {
        auto dirtProvider = std::make_shared<SimpleStateProvider>("minecraft:dirt");
        auto grassProvider = std::make_shared<SimpleStateProvider>("minecraft:grass_block");
        std::vector<RuleBasedBlockStateProvider::Rule> grassRules{
            {blockpredicates::BlockPredicate::not_(
                 blockpredicates::BlockPredicate::anyOf(
                     blockpredicates::BlockPredicate::solid(core::Vec3i(0, 1, 0)),
                     blockpredicates::BlockPredicate::matchesFluids(
                         core::Vec3i(0, 1, 0),
                         std::vector<std::string>{"minecraft:water"}))),
             grassProvider}
        };
        auto stateProvider = std::make_shared<RuleBasedBlockStateProvider>(
            dirtProvider, std::move(grassRules));
        s_stateProviders.push_back(stateProvider);

        auto targetPredicate = blockpredicates::BlockPredicate::matchesBlocks(
            std::vector<std::string>{"minecraft:dirt", "minecraft:mud"}
        );

        auto config = std::make_unique<DiskConfiguration>(
            stateProvider,
            targetPredicate,
            std::make_shared<util::UniformInt>(2, 6),  // radius
            2  // halfHeight
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<DiskConfiguration, DiskFeature>>(
            &s_diskFeature, *config);
        DISK_GRASS = feature.get();
        s_diskConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // BONUS_CHEST
    // Reference: MiscOverworldFeatures.java line 55
    // Feature.BONUS_CHEST with NoneFeatureConfiguration
    // =========================================================================
    {
        auto config = std::make_unique<NoneFeatureConfiguration>();
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, BonusChestFeature>>(
            &s_bonusChestFeature, *config);
        BONUS_CHEST = feature.get();
        s_noneConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // VOID_START_PLATFORM
    // Reference: MiscOverworldFeatures.java line 56
    // Feature.VOID_START_PLATFORM with NoneFeatureConfiguration
    // =========================================================================
    {
        auto config = std::make_unique<NoneFeatureConfiguration>();
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, VoidStartPlatformFeature>>(
            &s_voidStartPlatformFeature, *config);
        VOID_START_PLATFORM = feature.get();
        s_noneConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DESERT_WELL
    // Reference: 26.3 MiscOverworldFeatures.java - Overlay[
    //   Template(desert_well/well) at Offset(0, -2, 0),
    //   Template(desert_well/suspicious_sand, RuleProcessor suspicious_sand ->
    //     suspicious_sand + AppendLoot(archaeology/desert_well)) at one of
    //     Offset (0,-1,0), (-1,-1,0), (0,-1,1), (1,-1,0), (0,-1,-1),
    //   the same at one of the y = -2 offsets ]
    // =========================================================================
    {
        using namespace levelgen::placement;
        static TemplateFeature s_wellTemplateFeature;
        static OverlayFeature s_wellOverlayFeature;
        static std::deque<PlacedFeature> s_wellPlaced;
        static std::deque<levelgen::carver::ConstantInt> s_wellInts;
        static std::deque<OffsetPlacement> s_wellOffsets;
        static std::deque<RandomlySelectedPlacement> s_wellSelections;
        static std::vector<std::unique_ptr<TemplateFeatureConfiguration>> s_wellTemplateConfigs;
        static std::vector<std::unique_ptr<OverlayFeatureConfiguration>> s_wellOverlayConfigs;

        auto offset = [](int x, int y, int z) -> OffsetPlacement* {
            s_wellInts.emplace_back(x);
            const levelgen::carver::IntProvider* px = &s_wellInts.back();
            s_wellInts.emplace_back(y);
            const levelgen::carver::IntProvider* py = &s_wellInts.back();
            s_wellInts.emplace_back(z);
            s_wellOffsets.emplace_back(px, py, &s_wellInts.back());
            return &s_wellOffsets.back();
        };
        auto templateFeature = [&](const std::string& id, bool suspiciousSandLoot) -> ConfiguredFeature* {
            auto config = std::make_unique<TemplateFeatureConfiguration>(
                std::vector<TemplateFeatureConfiguration::Entry>{{id, 1, {0, 1, 2, 3}}});
            if (suspiciousSandLoot) {
                config->appendLootRules.push_back(
                    {"minecraft:suspicious_sand", "minecraft:archaeology/desert_well", "minecraft:brushable_block"});
            }
            s_wellTemplateConfigs.push_back(std::move(config));
            auto feature = std::make_unique<ConfiguredFeatureImpl<TemplateFeatureConfiguration, TemplateFeature>>(
                &s_wellTemplateFeature, *s_wellTemplateConfigs.back());
            ConfiguredFeature* raw = feature.get();
            s_features.push_back(std::move(feature));
            return raw;
        };
        auto suspiciousSand = [&](int y) -> PlacedFeature* {
            s_wellSelections.emplace_back(std::vector<PlacementModifier*>{
                offset(0, y, 0), offset(-1, y, 0), offset(0, y, 1), offset(1, y, 0), offset(0, y, -1)});
            s_wellPlaced.emplace_back(templateFeature("minecraft:desert_well/suspicious_sand", true),
                                      std::vector<PlacementModifier*>{&s_wellSelections.back()},
                                      "DESERT_WELL_SUSPICIOUS_SAND_INLINE");
            return &s_wellPlaced.back();
        };
        s_wellPlaced.emplace_back(templateFeature("minecraft:desert_well/well", false),
                                  std::vector<PlacementModifier*>{offset(0, -2, 0)}, "DESERT_WELL_WELL_INLINE");
        PlacedFeature* well = &s_wellPlaced.back();
        PlacedFeature* sandUpper = suspiciousSand(-1);
        PlacedFeature* sandLower = suspiciousSand(-2);

        s_wellOverlayConfigs.push_back(std::make_unique<OverlayFeatureConfiguration>(
            std::vector<PlacedFeature*>{well, sandUpper, sandLower}));
        auto feature = std::make_unique<ConfiguredFeatureImpl<OverlayFeatureConfiguration, OverlayFeature>>(
            &s_wellOverlayFeature, *s_wellOverlayConfigs.back());
        DESERT_WELL = feature.get();
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SPRING_LAVA_OVERWORLD
    // Reference: MiscOverworldFeatures.java line 58
    // SpringConfiguration(Fluids.LAVA, true, 4, 1, HolderSet.direct(
    //   STONE, GRANITE, DIORITE, ANDESITE, DEEPSLATE, TUFF, CALCITE, DIRT))
    // =========================================================================
    {
        auto config = std::make_unique<SpringConfiguration>(
            "minecraft:lava",  // fluid
            true,              // requiresBlockBelow
            4,                 // rockCount
            1,                 // holeCount
            std::set<std::string>{
                "minecraft:stone", "minecraft:granite", "minecraft:diorite",
                "minecraft:andesite", "minecraft:deepslate", "minecraft:tuff",
                "minecraft:calcite", "minecraft:dirt"
            }
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<SpringConfiguration, SpringFeature>>(
            &s_springFeature, *config);
        SPRING_LAVA_OVERWORLD = feature.get();
        s_springConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SPRING_LAVA_FROZEN
    // Reference: MiscOverworldFeatures.java line 59
    // SpringConfiguration(Fluids.LAVA, true, 4, 1, HolderSet.direct(
    //   SNOW_BLOCK, POWDER_SNOW, PACKED_ICE))
    // =========================================================================
    {
        auto config = std::make_unique<SpringConfiguration>(
            "minecraft:lava",  // fluid
            true,              // requiresBlockBelow
            4,                 // rockCount
            1,                 // holeCount
            std::set<std::string>{
                "minecraft:snow_block", "minecraft:powder_snow", "minecraft:packed_ice"
            }
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<SpringConfiguration, SpringFeature>>(
            &s_springFeature, *config);
        SPRING_LAVA_FROZEN = feature.get();
        s_springConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SPRING_WATER
    // Reference: MiscOverworldFeatures.java line 60
    // SpringConfiguration(Fluids.WATER, true, 4, 1, HolderSet.direct(
    //   STONE, GRANITE, DIORITE, ANDESITE, DEEPSLATE, TUFF, CALCITE, DIRT,
    //   SNOW_BLOCK, POWDER_SNOW, PACKED_ICE))
    // =========================================================================
    {
        auto config = std::make_unique<SpringConfiguration>(
            "minecraft:water",  // fluid
            true,               // requiresBlockBelow
            4,                  // rockCount
            1,                  // holeCount
            std::set<std::string>{
                "minecraft:stone", "minecraft:granite", "minecraft:diorite",
                "minecraft:andesite", "minecraft:deepslate", "minecraft:tuff",
                "minecraft:calcite", "minecraft:dirt", "minecraft:snow_block",
                "minecraft:powder_snow", "minecraft:packed_ice"
            }
        );
        auto feature = std::make_unique<ConfiguredFeatureImpl<SpringConfiguration, SpringFeature>>(
            &s_springFeature, *config);
        SPRING_WATER = feature.get();
        s_springConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
