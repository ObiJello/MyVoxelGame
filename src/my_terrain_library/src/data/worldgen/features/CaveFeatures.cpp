#include "data/worldgen/features/CaveFeatures.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "util/IntProvider.h"

// Reference: net/minecraft/data/worldgen/features/CaveFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;
using namespace levelgen;
using namespace levelgen::blockpredicates;
using namespace levelgen::placement;
using namespace levelgen::feature::stateproviders;
using Blocks = ::minecraft::world::level::block::Blocks;

// Static members
GeodeFeature CaveFeatures::s_geodeFeature;
MultifaceGrowthFeature CaveFeatures::s_multifaceGrowthFeature;
SimpleBlockFeature CaveFeatures::s_simpleBlockFeature;
VegetationPatchFeature CaveFeatures::s_vegetationPatchFeature;
BlockColumnFeature CaveFeatures::s_blockColumnFeature;
SimpleRandomSelectorFeature CaveFeatures::s_simpleRandomSelectorFeature;
UnderwaterMagmaFeature CaveFeatures::s_underwaterMagmaFeature;
SculkPatchFeature CaveFeatures::s_sculkPatchFeature;
MonsterRoomFeature CaveFeatures::s_monsterRoomFeature;
FossilFeature CaveFeatures::s_fossilFeature;
DripstoneClusterFeature CaveFeatures::s_dripstoneClusterFeature;
LargeDripstoneFeature CaveFeatures::s_largeDripstoneFeature;
PointedDripstoneFeature CaveFeatures::s_pointedDripstoneFeature;
RootSystemFeature CaveFeatures::s_rootSystemFeature;
RandomBooleanSelectorFeature CaveFeatures::s_randomBooleanSelectorFeature;
WaterloggedVegetationPatchFeature CaveFeatures::s_waterloggedVegPatchFeature;
bool CaveFeatures::s_initialized = false;

// ConfiguredFeature pointers
ConfiguredFeature* CaveFeatures::MONSTER_ROOM = nullptr;
ConfiguredFeature* CaveFeatures::FOSSIL_COAL = nullptr;
ConfiguredFeature* CaveFeatures::FOSSIL_DIAMONDS = nullptr;
ConfiguredFeature* CaveFeatures::DRIPSTONE_CLUSTER = nullptr;
ConfiguredFeature* CaveFeatures::LARGE_DRIPSTONE = nullptr;
ConfiguredFeature* CaveFeatures::POINTED_DRIPSTONE = nullptr;
ConfiguredFeature* CaveFeatures::UNDERWATER_MAGMA = nullptr;
ConfiguredFeature* CaveFeatures::GLOW_LICHEN = nullptr;
ConfiguredFeature* CaveFeatures::ROOTED_AZALEA_TREE = nullptr;
ConfiguredFeature* CaveFeatures::CAVE_VINE = nullptr;
ConfiguredFeature* CaveFeatures::CAVE_VINE_IN_MOSS = nullptr;
ConfiguredFeature* CaveFeatures::MOSS_VEGETATION = nullptr;
ConfiguredFeature* CaveFeatures::MOSS_PATCH = nullptr;
ConfiguredFeature* CaveFeatures::MOSS_PATCH_BONEMEAL = nullptr;
ConfiguredFeature* CaveFeatures::DRIPLEAF = nullptr;
ConfiguredFeature* CaveFeatures::CLAY_WITH_DRIPLEAVES = nullptr;
ConfiguredFeature* CaveFeatures::CLAY_POOL_WITH_DRIPLEAVES = nullptr;
ConfiguredFeature* CaveFeatures::LUSH_CAVES_CLAY = nullptr;
ConfiguredFeature* CaveFeatures::MOSS_PATCH_CEILING = nullptr;
ConfiguredFeature* CaveFeatures::SPORE_BLOSSOM = nullptr;
ConfiguredFeature* CaveFeatures::AMETHYST_GEODE = nullptr;
ConfiguredFeature* CaveFeatures::SCULK_PATCH_DEEP_DARK = nullptr;
ConfiguredFeature* CaveFeatures::SCULK_PATCH_ANCIENT_CITY = nullptr;
ConfiguredFeature* CaveFeatures::SCULK_VEIN = nullptr;

// Storage for ConfiguredFeature instances
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<GeodeConfiguration>> s_geodeConfigs;
static std::vector<std::unique_ptr<MultifaceGrowthConfiguration>> s_multifaceConfigs;
static std::vector<std::unique_ptr<SimpleBlockConfiguration>> s_simpleBlockConfigs;
static std::vector<std::unique_ptr<VegetationPatchConfiguration>> s_vegPatchConfigs;
static std::vector<std::unique_ptr<BlockColumnConfiguration>> s_blockColumnConfigs;
static std::vector<std::unique_ptr<SimpleRandomFeatureConfiguration>> s_simpleRandomConfigs;
static std::vector<std::unique_ptr<RandomBooleanFeatureConfiguration>> s_randomBooleanConfigs;
static std::vector<std::unique_ptr<RootSystemConfiguration>> s_rootSystemConfigs;
static std::vector<std::unique_ptr<UnderwaterMagmaConfiguration>> s_underwaterMagmaConfigs;
static std::vector<std::unique_ptr<SculkPatchConfiguration>> s_sculkPatchConfigs;

// Storage for SimpleStateProviders and WeightedStateProviders
static std::vector<std::unique_ptr<SimpleStateProvider>> s_stateProviders;
static std::vector<std::unique_ptr<WeightedStateProvider>> s_weightedStateProviders;
static std::vector<std::shared_ptr<BlockStateProvider>> s_blockStateProviders;
static std::vector<std::shared_ptr<RandomizedIntStateProvider>> s_randomizedIntStateProviders;

// Storage for PlacedFeatures (inline placed features for DRIPLEAF)
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;

// Static feature instances for dripleaf sub-features
static SimpleBlockFeature s_dripleafSimpleBlockFeature;
static BlockColumnFeature s_dripleafBlockColumnFeature;

void CaveFeatures::bootstrap() {
    if (s_initialized) return;
    TreeFeatures::bootstrap();

    auto createPlacedFeature = [](ConfiguredFeature* feature, const std::string& name = "") -> PlacedFeature* {
        auto placed = std::make_unique<PlacedFeature>(feature, std::vector<PlacementModifier*>{}, name);
        PlacedFeature* raw = placed.get();
        s_placedFeatures.push_back(std::move(placed));
        return raw;
    };

    auto createSimpleBlockConfiguredFeature = [&](std::shared_ptr<BlockStateProvider> provider) -> ConfiguredFeature* {
        s_blockStateProviders.push_back(std::move(provider));
        auto feature = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlockFeature,
            SimpleBlockConfiguration(s_blockStateProviders.back().get())
        );
        ConfiguredFeature* raw = feature.get();
        s_features.push_back(std::move(feature));
        return raw;
    };

    // =========================================================================
    // AMETHYST_GEODE
    // Reference: CaveFeatures.java line 121
    // =========================================================================
    {
        // Create block state providers using block name strings
        // Reference: GeodeBlockSettings in CaveFeatures.java
        // fillingProvider = AIR
        // innerLayerProvider = AMETHYST_BLOCK
        // alternateInnerLayerProvider = BUDDING_AMETHYST
        // middleLayerProvider = CALCITE
        // outerLayerProvider = SMOOTH_BASALT

        auto airProvider = std::make_unique<SimpleStateProvider>("minecraft:air");
        auto amethystProvider = std::make_unique<SimpleStateProvider>("minecraft:amethyst_block");
        auto buddingProvider = std::make_unique<SimpleStateProvider>("minecraft:budding_amethyst");
        auto calciteProvider = std::make_unique<SimpleStateProvider>("minecraft:calcite");
        auto smoothBasaltProvider = std::make_unique<SimpleStateProvider>("minecraft:smooth_basalt");

        // Inner placements (amethyst buds and cluster) - need BlockState* from Blocks
        std::vector<BlockState*> innerPlacements = {
            static_cast<BlockState*>(Blocks::getDefaultState("minecraft:small_amethyst_bud")),
            static_cast<BlockState*>(Blocks::getDefaultState("minecraft:medium_amethyst_bud")),
            static_cast<BlockState*>(Blocks::getDefaultState("minecraft:large_amethyst_bud")),
            static_cast<BlockState*>(Blocks::getDefaultState("minecraft:amethyst_cluster"))
        };

        // Create GeodeBlockSettings
        GeodeBlockSettings blockSettings;
        blockSettings.fillingProvider = std::shared_ptr<BlockStateProvider>(airProvider.get(), [](BlockStateProvider*){});
        blockSettings.innerLayerProvider = std::shared_ptr<BlockStateProvider>(amethystProvider.get(), [](BlockStateProvider*){});
        blockSettings.alternateInnerLayerProvider = std::shared_ptr<BlockStateProvider>(buddingProvider.get(), [](BlockStateProvider*){});
        blockSettings.middleLayerProvider = std::shared_ptr<BlockStateProvider>(calciteProvider.get(), [](BlockStateProvider*){});
        blockSettings.outerLayerProvider = std::shared_ptr<BlockStateProvider>(smoothBasaltProvider.get(), [](BlockStateProvider*){});
        blockSettings.innerPlacements = innerPlacements;
        blockSettings.cannotReplace = "minecraft:features_cannot_replace";
        blockSettings.invalidBlocks = "minecraft:geode_invalid_blocks";

        // Save providers
        s_stateProviders.push_back(std::move(airProvider));
        s_stateProviders.push_back(std::move(amethystProvider));
        s_stateProviders.push_back(std::move(buddingProvider));
        s_stateProviders.push_back(std::move(calciteProvider));
        s_stateProviders.push_back(std::move(smoothBasaltProvider));

        // Create layer settings
        // Reference: new GeodeLayerSettings(1.7, 2.2, 3.2, 4.2)
        GeodeLayerSettings layerSettings(1.7, 2.2, 3.2, 4.2);

        // Create crack settings
        // Reference: new GeodeCrackSettings(0.95, 2.0, 2)
        GeodeCrackSettings crackSettings(0.95, 2.0, 2);

        // Create IntProviders
        // outerWallDistance: UniformInt.of(4, 6)
        // distributionPoints: UniformInt.of(3, 4)
        // pointOffset: UniformInt.of(1, 2)
        auto outerWallDistance = std::make_shared<util::UniformInt>(4, 6);
        auto distributionPoints = std::make_shared<util::UniformInt>(3, 4);
        auto pointOffset = std::make_shared<util::UniformInt>(1, 2);

        // Create GeodeConfiguration
        // Reference: CaveFeatures.java line 121
        // 0.35 = usePotentialPlacementsChance
        // 0.083 = useAlternateLayer0Chance
        // true = placementsRequireLayer0Alternate
        // -16, 16 = minGenOffset, maxGenOffset
        // 0.05 = noiseMultiplier
        // 1 = invalidBlocksThreshold
        auto config = std::make_unique<GeodeConfiguration>(
            blockSettings,
            layerSettings,
            crackSettings,
            0.35,
            0.083,
            true,
            outerWallDistance,
            distributionPoints,
            pointOffset,
            -16,
            16,
            0.05,
            1
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<GeodeConfiguration, GeodeFeature>>(
            &s_geodeFeature, *config);
        AMETHYST_GEODE = feature.get();
        s_geodeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // GLOW_LICHEN
    // Reference: CaveFeatures.java line 106
    // MultifaceGrowthConfiguration(glowLichenBlock, 20, false, true, true, 0.5F,
    //   HolderSet.direct(Block::builtInRegistryHolder, Blocks.STONE, Blocks.ANDESITE,
    //   Blocks.DIORITE, Blocks.GRANITE, Blocks.DRIPSTONE_BLOCK, Blocks.CALCITE,
    //   Blocks.TUFF, Blocks.DEEPSLATE))
    // =========================================================================
    {
        auto config = std::make_unique<MultifaceGrowthConfiguration>();
        config->placeBlock = "minecraft:glow_lichen";
        config->searchRange = 20;
        config->canPlaceOnFloor = false;
        config->canPlaceOnWall = true;
        config->canPlaceOnCeiling = true;
        config->chanceOfSpreading = 0.5f;
        config->canBePlacedOn = {
            "minecraft:stone", "minecraft:andesite", "minecraft:diorite",
            "minecraft:granite", "minecraft:dripstone_block", "minecraft:calcite",
            "minecraft:tuff", "minecraft:deepslate"
        };

        auto feature = std::make_unique<ConfiguredFeatureImpl<MultifaceGrowthConfiguration, MultifaceGrowthFeature>>(
            &s_multifaceGrowthFeature, *config);
        GLOW_LICHEN = feature.get();
        s_multifaceConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // ROOTED_AZALEA_TREE
    // Reference: CaveFeatures.java line 107
    // =========================================================================
    {
        auto azaleaTree = createPlacedFeature(TreeFeatures::AZALEA_TREE, "AZALEA_TREE_INLINE");
        auto config = std::make_unique<RootSystemConfiguration>(
            azaleaTree,
            3,
            3,
            "minecraft:azalea_root_replaceable",
            BlockStateProvider::simple(Blocks::ROOTED_DIRT),
            20,
            100,
            3,
            2,
            BlockStateProvider::simple(Blocks::HANGING_ROOTS),
            20,
            2,
            BlockPredicate::allOf(
                BlockPredicate::anyOf(
                    BlockPredicate::matchesBlocks(
                        std::vector<std::string>{
                            "minecraft:air",
                            "minecraft:cave_air",
                            "minecraft:void_air"
                        }
                    ),
                    BlockPredicate::matchesTag("minecraft:replaceable_by_trees")
                ),
                BlockPredicate::matchesTag(core::Vec3i(0, -1, 0), "minecraft:azalea_grows_on")
            )
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<RootSystemConfiguration, RootSystemFeature>>(
            &s_rootSystemFeature, *config);
        ROOTED_AZALEA_TREE = feature.get();
        s_features.push_back(std::move(feature));
        s_blockStateProviders.push_back(config->rootStateProvider);
        s_blockStateProviders.push_back(config->hangingRootStateProvider);
        s_rootSystemConfigs.push_back(std::move(config));
    }

    // =========================================================================
    // SPORE_BLOSSOM
    // Reference: CaveFeatures.java line 120
    // SimpleBlockFeature with BlockStateProvider.simple(Blocks.SPORE_BLOSSOM)
    // =========================================================================
    {
        auto config = std::make_unique<SimpleBlockConfiguration>(
            static_cast<BlockState*>(Blocks::getDefaultState("minecraft:spore_blossom"))
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlockFeature, *config);
        SPORE_BLOSSOM = feature.get();
        s_simpleBlockConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // MOSS_VEGETATION
    // Reference: CaveFeatures.java line 112
    // =========================================================================
    {
        auto provider = std::make_shared<WeightedStateProvider>(
            std::vector<WeightedStateEntry>{
                {Blocks::FLOWERING_AZALEA->defaultBlockState(), 4},
                {Blocks::AZALEA->defaultBlockState(), 7},
                {Blocks::MOSS_CARPET->defaultBlockState(), 25},
                {Blocks::SHORT_GRASS->defaultBlockState(), 50},
                {Blocks::TALL_GRASS->defaultBlockState(), 10}
            }
        );

        MOSS_VEGETATION = createSimpleBlockConfiguredFeature(provider);
    }

    // =========================================================================
    // CAVE_VINE / CAVE_VINE_IN_MOSS
    // Reference: CaveFeatures.java lines 109-110
    // =========================================================================
    {
        auto caveVinesBodyProvider = std::make_shared<WeightedStateProvider>(
            std::vector<WeightedStateEntry>{
                {Blocks::CAVE_VINES_PLANT->defaultBlockState(), 4},
                {Blocks::CAVE_VINES_PLANT->defaultBlockState()->setValue(*BlockStateProperties::BERRIES, true), 1}
            }
        );
        auto caveVinesHeadBaseProvider = std::make_shared<WeightedStateProvider>(
            std::vector<WeightedStateEntry>{
                {Blocks::CAVE_VINES->defaultBlockState(), 4},
                {Blocks::CAVE_VINES->defaultBlockState()->setValue(*BlockStateProperties::BERRIES, true), 1}
            }
        );
        auto caveVinesHeadProvider = std::make_shared<RandomizedIntStateProvider>(
            caveVinesHeadBaseProvider,
            "age",
            23,
            25
        );

        s_blockStateProviders.push_back(caveVinesBodyProvider);
        s_blockStateProviders.push_back(caveVinesHeadBaseProvider);
        s_randomizedIntStateProviders.push_back(caveVinesHeadProvider);

        {
            std::vector<BlockColumnConfiguration::Layer> layers = {
                BlockColumnConfiguration::layer(
                    util::WeightedListInt::builder()
                        .add(std::make_shared<util::UniformInt>(0, 19), 2)
                        .add(std::make_shared<util::UniformInt>(0, 2), 3)
                        .add(std::make_shared<util::UniformInt>(0, 6), 10)
                        .buildShared(),
                    caveVinesBodyProvider
                ),
                BlockColumnConfiguration::layer(
                    std::make_shared<util::ConstantInt>(1),
                    caveVinesHeadProvider
                )
            };

            auto config = std::make_unique<BlockColumnConfiguration>(
                layers,
                core::Direction::DOWN,
                BlockPredicate::ONLY_IN_AIR_PREDICATE,
                true
            );

            auto feature = std::make_unique<ConfiguredFeatureImpl<BlockColumnConfiguration, BlockColumnFeature>>(
                &s_blockColumnFeature, *config);
            CAVE_VINE = feature.get();
            s_blockColumnConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }

        {
            std::vector<BlockColumnConfiguration::Layer> layers = {
                BlockColumnConfiguration::layer(
                    util::WeightedListInt::builder()
                        .add(std::make_shared<util::UniformInt>(0, 3), 5)
                        .add(std::make_shared<util::UniformInt>(1, 7), 1)
                        .buildShared(),
                    caveVinesBodyProvider
                ),
                BlockColumnConfiguration::layer(
                    std::make_shared<util::ConstantInt>(1),
                    caveVinesHeadProvider
                )
            };

            auto config = std::make_unique<BlockColumnConfiguration>(
                layers,
                core::Direction::DOWN,
                BlockPredicate::ONLY_IN_AIR_PREDICATE,
                true
            );

            auto feature = std::make_unique<ConfiguredFeatureImpl<BlockColumnConfiguration, BlockColumnFeature>>(
                &s_blockColumnFeature, *config);
            CAVE_VINE_IN_MOSS = feature.get();
            s_blockColumnConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // MOSS_PATCH / MOSS_PATCH_BONEMEAL
    // Reference: CaveFeatures.java lines 113-114
    // =========================================================================
    {
        auto mossGroundState = BlockStateProvider::simple(Blocks::MOSS_BLOCK);
        PlacedFeature* mossVegetation = createPlacedFeature(MOSS_VEGETATION, "MOSS_VEGETATION_INLINE");

        auto config = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:moss_replaceable",
            mossGroundState,
            mossVegetation,
            CaveSurface::FLOOR,
            std::make_shared<util::ConstantInt>(1),
            0.0f,
            5,
            0.8f,
            std::make_shared<util::UniformInt>(4, 7),
            0.3f
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, VegetationPatchFeature>>(
            &s_vegetationPatchFeature, *config);
        MOSS_PATCH = feature.get();
        s_vegPatchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    {
        auto mossGroundState = BlockStateProvider::simple(Blocks::MOSS_BLOCK);
        PlacedFeature* mossVegetation = createPlacedFeature(MOSS_VEGETATION, "MOSS_VEGETATION_BONEMEAL_INLINE");

        auto config = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:moss_replaceable",
            mossGroundState,
            mossVegetation,
            CaveSurface::FLOOR,
            std::make_shared<util::ConstantInt>(1),
            0.0f,
            5,
            0.6f,
            std::make_shared<util::UniformInt>(1, 2),
            0.75f
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, VegetationPatchFeature>>(
            &s_vegetationPatchFeature, *config);
        MOSS_PATCH_BONEMEAL = feature.get();
        s_vegPatchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DRIPLEAF
    // Reference: CaveFeatures.java line 115
    // =========================================================================
    {
        std::vector<PlacedFeature*> dripleafFeatures;

        {
            auto provider = std::make_shared<WeightedStateProvider>(
                std::vector<WeightedStateEntry>{
                    {Blocks::SMALL_DRIPLEAF->defaultBlockState()->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::EAST), 1},
                    {Blocks::SMALL_DRIPLEAF->defaultBlockState()->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::WEST), 1},
                    {Blocks::SMALL_DRIPLEAF->defaultBlockState()->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH), 1},
                    {Blocks::SMALL_DRIPLEAF->defaultBlockState()->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::SOUTH), 1}
                }
            );
            ConfiguredFeature* configured = createSimpleBlockConfiguredFeature(provider);
            dripleafFeatures.push_back(createPlacedFeature(configured, "SMALL_DRIPLEAF_INLINE"));
        }

        for (core::Direction direction : {core::Direction::EAST, core::Direction::WEST, core::Direction::SOUTH, core::Direction::NORTH}) {
            auto stemProvider = BlockStateProvider::simple(
                Blocks::BIG_DRIPLEAF_STEM->defaultBlockState()->setValue(*BlockStateProperties::HORIZONTAL_FACING, direction)
            );
            auto headProvider = BlockStateProvider::simple(
                Blocks::BIG_DRIPLEAF->defaultBlockState()->setValue(*BlockStateProperties::HORIZONTAL_FACING, direction)
            );
            s_blockStateProviders.push_back(stemProvider);
            s_blockStateProviders.push_back(headProvider);

            std::vector<BlockColumnConfiguration::Layer> layers = {
                BlockColumnConfiguration::layer(
                    util::WeightedListInt::builder()
                        .add(std::make_shared<util::UniformInt>(0, 4), 2)
                        .add(std::make_shared<util::ConstantInt>(0), 1)
                        .buildShared(),
                    stemProvider
                ),
                BlockColumnConfiguration::layer(
                    std::make_shared<util::ConstantInt>(1),
                    headProvider
                )
            };

            auto config = std::make_unique<BlockColumnConfiguration>(
                layers,
                core::Direction::UP,
                BlockPredicate::ONLY_IN_AIR_OR_WATER_PREDICATE,
                true
            );

            auto feature = std::make_unique<ConfiguredFeatureImpl<BlockColumnConfiguration, BlockColumnFeature>>(
                &s_blockColumnFeature, *config);
            ConfiguredFeature* configured = feature.get();
            s_blockColumnConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
            dripleafFeatures.push_back(createPlacedFeature(configured, "BIG_DRIPLEAF_INLINE"));
        }

        auto config = std::make_unique<SimpleRandomFeatureConfiguration>(dripleafFeatures);
        auto feature = std::make_unique<ConfiguredFeatureImpl<SimpleRandomFeatureConfiguration, SimpleRandomSelectorFeature>>(
            &s_simpleRandomSelectorFeature, *config);
        DRIPLEAF = feature.get();
        s_simpleRandomConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // CLAY_WITH_DRIPLEAVES / CLAY_POOL_WITH_DRIPLEAVES / LUSH_CAVES_CLAY
    // Reference: CaveFeatures.java lines 116-118
    // =========================================================================
    {
        auto clayGroundState = BlockStateProvider::simple(Blocks::CLAY);
        PlacedFeature* dripleafPlaced = createPlacedFeature(DRIPLEAF, "DRIPLEAF_INLINE");

        auto clayConfig = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:lush_ground_replaceable",
            clayGroundState,
            dripleafPlaced,
            CaveSurface::FLOOR,
            std::make_shared<util::ConstantInt>(3),
            0.8f,
            2,
            0.05f,
            std::make_shared<util::UniformInt>(4, 7),
            0.7f
        );

        auto clayFeature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, VegetationPatchFeature>>(
            &s_vegetationPatchFeature, *clayConfig);
        CLAY_WITH_DRIPLEAVES = clayFeature.get();
        s_vegPatchConfigs.push_back(std::move(clayConfig));
        s_features.push_back(std::move(clayFeature));

        auto clayPoolConfig = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:lush_ground_replaceable",
            BlockStateProvider::simple(Blocks::CLAY),
            createPlacedFeature(DRIPLEAF, "DRIPLEAF_WATERLOGGED_INLINE"),
            CaveSurface::FLOOR,
            std::make_shared<util::ConstantInt>(3),
            0.8f,
            5,
            0.1f,
            std::make_shared<util::UniformInt>(4, 7),
            0.7f
        );

        auto clayPoolFeature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, WaterloggedVegetationPatchFeature>>(
            &s_waterloggedVegPatchFeature, *clayPoolConfig);
        CLAY_POOL_WITH_DRIPLEAVES = clayPoolFeature.get();
        s_vegPatchConfigs.push_back(std::move(clayPoolConfig));
        s_features.push_back(std::move(clayPoolFeature));

        auto lushCavesClayConfig = std::make_unique<RandomBooleanFeatureConfiguration>(
            createPlacedFeature(CLAY_WITH_DRIPLEAVES, "CLAY_WITH_DRIPLEAVES_INLINE"),
            createPlacedFeature(CLAY_POOL_WITH_DRIPLEAVES, "CLAY_POOL_WITH_DRIPLEAVES_INLINE")
        );

        auto lushCavesClayFeature = std::make_unique<ConfiguredFeatureImpl<RandomBooleanFeatureConfiguration, RandomBooleanSelectorFeature>>(
            &s_randomBooleanSelectorFeature, *lushCavesClayConfig);
        LUSH_CAVES_CLAY = lushCavesClayFeature.get();
        s_randomBooleanConfigs.push_back(std::move(lushCavesClayConfig));
        s_features.push_back(std::move(lushCavesClayFeature));
    }

    // =========================================================================
    // MOSS_PATCH_CEILING
    // Reference: CaveFeatures.java line 119
    // =========================================================================
    {
        auto config = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:moss_replaceable",
            BlockStateProvider::simple(Blocks::MOSS_BLOCK),
            createPlacedFeature(CAVE_VINE_IN_MOSS, "CAVE_VINE_IN_MOSS_INLINE"),
            CaveSurface::CEILING,
            std::make_shared<util::UniformInt>(1, 2),
            0.0f,
            5,
            0.08f,
            std::make_shared<util::UniformInt>(4, 7),
            0.3f
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, VegetationPatchFeature>>(
            &s_vegetationPatchFeature, *config);
        MOSS_PATCH_CEILING = feature.get();
        s_vegPatchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // UNDERWATER_MAGMA
    // Reference: CaveFeatures.java line 104
    // UnderwaterMagmaConfiguration(5, 1, 0.5F)
    // Places magma blocks on underwater floors
    // =========================================================================
    {
        auto config = std::make_unique<UnderwaterMagmaConfiguration>(
            5,    // floorSearchRange
            1,    // placementRadiusAroundFloor
            0.5f  // placementProbabilityPerValidPosition
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<UnderwaterMagmaConfiguration, UnderwaterMagmaFeature>>(
            &s_underwaterMagmaFeature, *config);
        UNDERWATER_MAGMA = feature.get();
        s_underwaterMagmaConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SCULK_PATCH_DEEP_DARK
    // Reference: CaveFeatures.java line 122
    // SculkPatchConfiguration(10, 32, 64, 0, 1, ConstantInt.of(0), 0.5F)
    // =========================================================================
    {
        auto config = std::make_unique<SculkPatchConfiguration>();
        config->chargeCount = 10;
        config->amountPerCharge = 32;
        config->spreadAttempts = 64;
        config->growthRounds = 0;
        config->spreadRounds = 1;
        config->extraRareGrowths = std::make_shared<util::ConstantInt>(0);
        config->catalystChance = 0.5f;

        auto feature = std::make_unique<ConfiguredFeatureImpl<SculkPatchConfiguration, SculkPatchFeature>>(
            &s_sculkPatchFeature, *config);
        SCULK_PATCH_DEEP_DARK = feature.get();
        s_sculkPatchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SCULK_PATCH_ANCIENT_CITY
    // Reference: CaveFeatures.java line 123
    // SculkPatchConfiguration(10, 32, 64, 0, 1, UniformInt.of(1, 3), 0.5F)
    // =========================================================================
    {
        auto config = std::make_unique<SculkPatchConfiguration>();
        config->chargeCount = 10;
        config->amountPerCharge = 32;
        config->spreadAttempts = 64;
        config->growthRounds = 0;
        config->spreadRounds = 1;
        config->extraRareGrowths = std::make_shared<util::UniformInt>(1, 3);
        config->catalystChance = 0.5f;

        auto feature = std::make_unique<ConfiguredFeatureImpl<SculkPatchConfiguration, SculkPatchFeature>>(
            &s_sculkPatchFeature, *config);
        SCULK_PATCH_ANCIENT_CITY = feature.get();
        s_sculkPatchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SCULK_VEIN
    // Reference: CaveFeatures.java lines 124-125
    // MultifaceGrowthConfiguration(sculkVeinBlock, 20, true, true, true, 1.0F,
    //   HolderSet.direct(Block::builtInRegistryHolder, Blocks.STONE, Blocks.ANDESITE,
    //   Blocks.DIORITE, Blocks.GRANITE, Blocks.DRIPSTONE_BLOCK, Blocks.CALCITE,
    //   Blocks.TUFF, Blocks.DEEPSLATE))
    // =========================================================================
    {
        auto config = std::make_unique<MultifaceGrowthConfiguration>();
        config->placeBlock = "minecraft:sculk_vein";
        config->searchRange = 20;
        config->canPlaceOnFloor = true;
        config->canPlaceOnCeiling = true;
        config->canPlaceOnWall = true;
        config->chanceOfSpreading = 1.0f;
        config->canBePlacedOn = {
            "minecraft:stone", "minecraft:andesite", "minecraft:diorite",
            "minecraft:granite", "minecraft:dripstone_block", "minecraft:calcite",
            "minecraft:tuff", "minecraft:deepslate"
        };

        auto feature = std::make_unique<ConfiguredFeatureImpl<MultifaceGrowthConfiguration, MultifaceGrowthFeature>>(
            &s_multifaceGrowthFeature, *config);
        SCULK_VEIN = feature.get();
        s_multifaceConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // MONSTER_ROOM (Dungeon)
    // Reference: CaveFeatures.java line 95
    // MonsterRoomFeature with NoneFeatureConfiguration
    // =========================================================================
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, MonsterRoomFeature>>(
            &s_monsterRoomFeature, NoneFeatureConfiguration::INSTANCE);
        MONSTER_ROOM = feature.get();
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DRIPSTONE_CLUSTER
    // Reference: CaveFeatures.java line 101
    // DripstoneClusterConfiguration(12, UniformInt(3,6), UniformInt(2,8), 1, 3,
    //   UniformInt(2,4), UniformFloat(0.3,0.7), ClampedNormalFloat(0.1,0.3,0.1,0.9),
    //   0.1, 3, 8)
    // =========================================================================
    {
        static DripstoneClusterFeature s_dripClusterFeature;
        auto config = std::make_unique<DripstoneClusterConfiguration>();
        config->floorToCeilingSearchRange = 12;
        config->height = std::make_shared<::minecraft::util::UniformInt>(3, 6);
        config->radius = std::make_shared<::minecraft::util::UniformInt>(2, 8);
        config->maxStalagmiteStalactiteHeightDiff = 1;
        config->heightDeviation = 3;
        config->dripstoneBlockLayerThickness = std::make_shared<::minecraft::util::UniformInt>(2, 4);
        config->wetness = std::make_shared<levelgen::carver::UniformFloat>(0.3f, 0.7f);
        config->density = std::make_shared<levelgen::carver::ClampedNormalFloat>(0.1f, 0.3f, 0.1f, 0.9f);
        config->chanceOfDripstoneColumnAtMaxDistanceFromCenter = 0.1f;
        config->maxDistanceFromEdgeAffectingChanceOfDripstoneColumn = 3;
        config->maxDistanceFromCenterAffectingHeightBias = 8;
        auto feature = std::make_unique<ConfiguredFeatureImpl<DripstoneClusterConfiguration, DripstoneClusterFeature>>(
            &s_dripClusterFeature, *config);
        DRIPSTONE_CLUSTER = feature.get();
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // LARGE_DRIPSTONE
    // Reference: CaveFeatures.java line 102
    // LargeDripstoneConfiguration(30, UniformInt(3,19), UniformFloat(0.4,2.0), 0.33,
    //   UniformFloat(0.3,0.9), UniformFloat(0.4,1.0), UniformFloat(0.0,0.3), 4, 0.6)
    // =========================================================================
    {
        static LargeDripstoneFeature s_largeDripFeature;
        auto config = std::make_unique<LargeDripstoneConfiguration>();
        config->floorToCeilingSearchRange = 30;
        config->columnRadius = std::make_shared<::minecraft::util::UniformInt>(3, 19);
        config->maxColumnRadiusToCaveHeightRatio = 0.33f;
        config->stalactiteBluntness = std::make_shared<levelgen::carver::UniformFloat>(0.3f, 0.9f);
        config->stalagmiteBluntness = std::make_shared<levelgen::carver::UniformFloat>(0.4f, 1.0f);
        config->heightScale = std::make_shared<levelgen::carver::UniformFloat>(0.4f, 2.0f);
        config->windSpeed = std::make_shared<levelgen::carver::UniformFloat>(0.0f, 0.3f);
        config->minRadiusForWind = 4;
        config->minBluntnessForWind = 0.6f;
        auto feature = std::make_unique<ConfiguredFeatureImpl<LargeDripstoneConfiguration, LargeDripstoneFeature>>(
            &s_largeDripFeature, *config);
        LARGE_DRIPSTONE = feature.get();
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // POINTED_DRIPSTONE
    // Reference: CaveFeatures.java line 103
    // PointedDripstoneConfiguration(0.2, 0.7, 0.5, 0.5)
    // Note: Java wraps this in SimpleRandomSelector with EnvironmentScan placement,
    // but we register the inner feature directly since placement handles the scanning
    // =========================================================================
    {
        static PointedDripstoneFeature s_pointedDripFeature;
        auto config = std::make_unique<PointedDripstoneConfiguration>(0.2f, 0.7f, 0.5f, 0.5f);
        auto feature = std::make_unique<ConfiguredFeatureImpl<PointedDripstoneConfiguration, PointedDripstoneFeature>>(
            &s_pointedDripFeature, *config);
        POINTED_DRIPSTONE = feature.get();
        s_features.push_back(std::move(feature));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
