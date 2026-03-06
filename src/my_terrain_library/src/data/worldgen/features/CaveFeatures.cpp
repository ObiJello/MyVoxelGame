#include "data/worldgen/features/CaveFeatures.h"
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
static std::vector<std::unique_ptr<UnderwaterMagmaConfiguration>> s_underwaterMagmaConfigs;
static std::vector<std::unique_ptr<SculkPatchConfiguration>> s_sculkPatchConfigs;

// Storage for SimpleStateProviders and WeightedStateProviders
static std::vector<std::unique_ptr<SimpleStateProvider>> s_stateProviders;
static std::vector<std::unique_ptr<WeightedStateProvider>> s_weightedStateProviders;

// Storage for PlacedFeatures (inline placed features for DRIPLEAF)
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;

// Static feature instances for dripleaf sub-features
static SimpleBlockFeature s_dripleafSimpleBlockFeature;
static BlockColumnFeature s_dripleafBlockColumnFeature;

void CaveFeatures::bootstrap() {
    if (s_initialized) return;

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
    // MOSS_PATCH
    // Reference: CaveFeatures.java line 113
    // VegetationPatchFeature with MOSS_BLOCK as ground state
    // VegetationPatchConfiguration(BlockTags.MOSS_REPLACEABLE, MOSS_BLOCK,
    //   MOSS_VEGETATION, CaveSurface.FLOOR, depth=1, extraBottomChance=0.0,
    //   verticalRange=5, vegetationChance=0.8, xzRadius=4-7, extraEdgeChance=0.3)
    // =========================================================================
    {
        auto groundState = std::make_shared<SimpleStateProvider>("minecraft:moss_block");
        s_stateProviders.push_back(std::make_unique<SimpleStateProvider>("minecraft:moss_block"));

        auto config = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:moss_replaceable",  // replaceable tag
            groundState,                    // ground state (moss_block)
            CaveSurface::FLOOR,            // surface type
            std::make_shared<util::ConstantInt>(1),  // depth
            0.0f,                          // extraBottomBlockChance
            5,                             // verticalRange
            0.8f,                          // vegetationChance
            std::make_shared<util::UniformInt>(4, 7),  // xzRadius
            0.3f                           // extraEdgeColumnChance
        );

        // Set vegetation placer to place moss_carpet and grass
        config->vegetationPlacer = [](WorldGenLevel* level, ChunkGenerator* gen,
                                      WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            // Weighted selection: moss_carpet(25), short_grass(50), tall_grass(10)
            // azalea(7), flowering_azalea(4)
            int roll = random.nextInt(96);
            BlockState* state = nullptr;
            if (roll < 50) {
                state = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:short_grass"));
            } else if (roll < 75) {
                state = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:moss_carpet"));
            } else if (roll < 85) {
                state = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:tall_grass"));
            } else if (roll < 92) {
                state = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:azalea"));
            } else {
                state = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:flowering_azalea"));
            }
            if (state) {
                level->setBlock(pos, state, 2);
                return true;
            }
            return false;
        };

        auto feature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, VegetationPatchFeature>>(
            &s_vegetationPatchFeature, *config);
        MOSS_PATCH = feature.get();
        s_vegPatchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // CAVE_VINE
    // Reference: CaveFeatures.java line 110
    // BlockColumnFeature that places cave_vines hanging down
    // =========================================================================
    {
        // Create layers: body (cave_vines_plant) and tip (cave_vines)
        std::vector<BlockColumnConfiguration::Layer> layers;
        layers.push_back(BlockColumnConfiguration::layer(
            std::make_shared<util::UniformInt>(1, 7),
            std::make_shared<SimpleStateProvider>("minecraft:cave_vines_plant")
        ));
        layers.push_back(BlockColumnConfiguration::layer(
            std::make_shared<util::ConstantInt>(1),
            std::make_shared<SimpleStateProvider>("minecraft:cave_vines")
        ));

        auto config = std::make_unique<BlockColumnConfiguration>(
            layers,
            core::Direction::DOWN,
            nullptr,  // allowedPlacement - would be BlockPredicate.ONLY_IN_AIR_PREDICATE
            true      // prioritizeTip
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<BlockColumnConfiguration, BlockColumnFeature>>(
            &s_blockColumnFeature, *config);
        CAVE_VINE = feature.get();
        s_blockColumnConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DRIPLEAF
    // Reference: CaveFeatures.java line 115
    // SimpleRandomSelectorFeature with 5 inline placed features:
    //   - makeSmallDripleaf() - equal weight random facing
    //   - makeDripleaf(EAST), makeDripleaf(WEST), makeDripleaf(SOUTH), makeDripleaf(NORTH)
    // =========================================================================
    {
        // Create 5 inline PlacedFeatures for DRIPLEAF
        std::vector<PlacedFeature*> dripleafFeatures;

        // 1. Small dripleaf - uses WeightedStateProvider with 4 directions, each weight 1
        // Reference: CaveFeatures.java makeSmallDripleaf() lines 88-90
        {
            std::vector<WeightedStateEntry> smallDripleafStates = {
                WeightedStateEntry(static_cast<BlockState*>(Blocks::getDefaultState("minecraft:small_dripleaf")), 1),
                WeightedStateEntry(static_cast<BlockState*>(Blocks::getDefaultState("minecraft:small_dripleaf")), 1),
                WeightedStateEntry(static_cast<BlockState*>(Blocks::getDefaultState("minecraft:small_dripleaf")), 1),
                WeightedStateEntry(static_cast<BlockState*>(Blocks::getDefaultState("minecraft:small_dripleaf")), 1)
            };
            auto smallDripleafProvider = std::make_unique<WeightedStateProvider>(smallDripleafStates);

            auto smallDripleafConfig = std::make_unique<SimpleBlockConfiguration>(smallDripleafProvider.get());

            auto smallDripleafConfiguredFeature = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
                &s_dripleafSimpleBlockFeature, *smallDripleafConfig);

            // Create PlacedFeature with no modifiers (inlinePlaced)
            auto smallDripleafPlaced = std::make_unique<PlacedFeature>(
                smallDripleafConfiguredFeature.get(),
                std::vector<PlacementModifier*>{}
            );

            dripleafFeatures.push_back(smallDripleafPlaced.get());
            s_weightedStateProviders.push_back(std::move(smallDripleafProvider));
            s_simpleBlockConfigs.push_back(std::move(smallDripleafConfig));
            s_features.push_back(std::move(smallDripleafConfiguredFeature));
            s_placedFeatures.push_back(std::move(smallDripleafPlaced));
        }

        // 2-5. Big dripleaf for each direction (EAST, WEST, SOUTH, NORTH)
        // Reference: CaveFeatures.java makeDripleaf(Direction) lines 84-86
        // WeightedListInt: weight 2 for UniformInt(0,4), weight 1 for ConstantInt(0)
        // Layer 1: stem with weighted length
        // Layer 2: head with constant 1
        for (int dir = 0; dir < 4; ++dir) {
            // Create WeightedListInt for stem height
            // Reference: new WeightedListInt(WeightedList.builder().add(UniformInt.of(0, 4), 2).add(ConstantInt.of(0), 1).build())
            auto stemHeightProvider = util::WeightedListInt::builder()
                .add(std::make_shared<util::UniformInt>(0, 4), 2)
                .add(std::make_shared<util::ConstantInt>(0), 1)
                .buildShared();

            // Create layers
            std::vector<BlockColumnConfiguration::Layer> bigDripleafLayers;

            // Layer 1: BIG_DRIPLEAF_STEM (with weighted height)
            bigDripleafLayers.push_back(BlockColumnConfiguration::layer(
                stemHeightProvider,
                std::make_shared<SimpleStateProvider>("minecraft:big_dripleaf_stem")
            ));

            // Layer 2: BIG_DRIPLEAF (constant 1)
            bigDripleafLayers.push_back(BlockColumnConfiguration::layer(
                std::make_shared<util::ConstantInt>(1),
                std::make_shared<SimpleStateProvider>("minecraft:big_dripleaf")
            ));

            auto bigDripleafConfig = std::make_unique<BlockColumnConfiguration>(
                bigDripleafLayers,
                core::Direction::UP,  // Direction.UP
                nullptr,              // BlockPredicate.ONLY_IN_AIR_OR_WATER_PREDICATE
                true                  // prioritizeTip
            );

            auto bigDripleafConfiguredFeature = std::make_unique<ConfiguredFeatureImpl<BlockColumnConfiguration, BlockColumnFeature>>(
                &s_dripleafBlockColumnFeature, *bigDripleafConfig);

            auto bigDripleafPlaced = std::make_unique<PlacedFeature>(
                bigDripleafConfiguredFeature.get(),
                std::vector<PlacementModifier*>{}
            );

            dripleafFeatures.push_back(bigDripleafPlaced.get());
            s_blockColumnConfigs.push_back(std::move(bigDripleafConfig));
            s_features.push_back(std::move(bigDripleafConfiguredFeature));
            s_placedFeatures.push_back(std::move(bigDripleafPlaced));
        }

        // Create SimpleRandomFeatureConfiguration with all 5 features
        auto dripleafConfig = std::make_unique<SimpleRandomFeatureConfiguration>(dripleafFeatures);

        auto dripleafFeature = std::make_unique<ConfiguredFeatureImpl<SimpleRandomFeatureConfiguration, SimpleRandomSelectorFeature>>(
            &s_simpleRandomSelectorFeature, *dripleafConfig);
        DRIPLEAF = dripleafFeature.get();
        s_simpleRandomConfigs.push_back(std::move(dripleafConfig));
        s_features.push_back(std::move(dripleafFeature));
    }

    // =========================================================================
    // CLAY_WITH_DRIPLEAVES
    // Reference: CaveFeatures.java line 116
    // VegetationPatchFeature with CLAY ground and DRIPLEAF vegetation
    // VegetationPatchConfiguration(BlockTags.LUSH_GROUND_REPLACEABLE, CLAY,
    //   DRIPLEAF, CaveSurface.FLOOR, depth=3, extraBottomChance=0.8,
    //   verticalRange=2, vegetationChance=0.05, xzRadius=4-7, extraEdgeChance=0.7)
    // =========================================================================
    {
        auto clayGroundState = std::make_shared<SimpleStateProvider>("minecraft:clay");

        auto clayConfig = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:lush_ground_replaceable",  // replaceable tag
            clayGroundState,                       // ground state (clay)
            CaveSurface::FLOOR,                   // surface type
            std::make_shared<util::ConstantInt>(3),  // depth
            0.8f,                                 // extraBottomBlockChance
            2,                                    // verticalRange
            0.05f,                                // vegetationChance
            std::make_shared<util::UniformInt>(4, 7),  // xzRadius
            0.7f                                  // extraEdgeColumnChance
        );

        // Set vegetation placer to call DRIPLEAF feature
        // Reference: Java uses PlacementUtils.inlinePlaced(configuredFeatures.getOrThrow(DRIPLEAF))
        clayConfig->vegetationPlacer = [](WorldGenLevel* level, ChunkGenerator* gen,
                                          WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            // DRIPLEAF is a SimpleRandomSelectorFeature that randomly selects
            // from small dripleaf (weighted) or big dripleaf (4 directions)
            if (!DRIPLEAF) return false;

            // Place dripleaf directly matching the same random consumption pattern
            int featureIndex = random.nextInt(5);  // 5 sub-features
            if (featureIndex == 0) {
                // Small dripleaf - consume 1 random for weighted state selection
                random.nextInt(4);  // 4 equal weight entries
                BlockState* state = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:small_dripleaf"));
                if (state) {
                    level->setBlock(pos, state, 2);
                    return true;
                }
            } else {
                // Big dripleaf - consume random for WeightedListInt then place stem + head
                // WeightedListInt: weight 2 for UniformInt(0,4), weight 1 for ConstantInt(0)
                int weightSelect = random.nextInt(3);  // total weight = 3
                int stemHeight;
                if (weightSelect < 2) {
                    // UniformInt(0, 4) - range is 5
                    stemHeight = random.nextInt(5);  // 0-4 inclusive
                } else {
                    // ConstantInt(0)
                    stemHeight = 0;
                }

                // Place stem blocks
                core::BlockPos::MutableBlockPos placePos(pos.getX(), pos.getY(), pos.getZ());
                BlockState* stemState = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:big_dripleaf_stem"));
                for (int i = 0; i < stemHeight; ++i) {
                    if (stemState) {
                        level->setBlock(placePos, stemState, 2);
                    }
                    placePos.move(0, 1, 0);
                }

                // Place head
                BlockState* headState = static_cast<BlockState*>(Blocks::getDefaultState("minecraft:big_dripleaf"));
                if (headState) {
                    level->setBlock(placePos, headState, 2);
                }
                return true;
            }
            return false;
        };

        auto clayFeature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, VegetationPatchFeature>>(
            &s_vegetationPatchFeature, *clayConfig);
        CLAY_WITH_DRIPLEAVES = clayFeature.get();
        s_vegPatchConfigs.push_back(std::move(clayConfig));
        s_features.push_back(std::move(clayFeature));
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
    // SCULK_VEIN
    // Reference: CaveFeatures.java line 125
    // MultifaceGrowthConfiguration for sculk vein
    // =========================================================================
    {
        auto config = std::make_unique<MultifaceGrowthConfiguration>();
        config->searchRange = 20;
        config->canPlaceOnFloor = true;
        config->canPlaceOnCeiling = true;
        config->canPlaceOnWall = true;
        config->chanceOfSpreading = 1.0f;
        // Simplified: would add canBePlacedOn blocks list

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
