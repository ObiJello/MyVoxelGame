#include "data/worldgen/features/MiscOverworldFeatures.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "util/IntProvider.h"

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
DesertWellFeature MiscOverworldFeatures::s_desertWellFeature;
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
        auto stateProvider = std::make_shared<RuleBasedBlockStateProvider>(sandProvider);
        // TODO: Add rule for SANDSTONE below AIR
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
        auto stateProvider = std::make_shared<RuleBasedBlockStateProvider>(dirtProvider);
        // TODO: Add rule for GRASS_BLOCK when not solid/water above
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
    // Reference: MiscOverworldFeatures.java line 57
    // Feature.DESERT_WELL with NoneFeatureConfiguration
    // =========================================================================
    {
        auto config = std::make_unique<NoneFeatureConfiguration>();
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, DesertWellFeature>>(
            &s_desertWellFeature, *config);
        DESERT_WELL = feature.get();
        s_noneConfigs.push_back(std::move(config));
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
