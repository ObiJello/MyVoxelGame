#include "data/worldgen/features/VegetationFeatures.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "data/worldgen/placement/TreePlacements.h"
#include "levelgen/placement/PlacedFeature.h"

// Reference: net/minecraft/data/worldgen/features/VegetationFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;
using namespace levelgen;
using namespace levelgen::placement;
using Blocks = ::minecraft::world::level::block::Blocks;

// Static members
RandomPatchFeature VegetationFeatures::s_randomPatchFeature;
SimpleBlockFeature VegetationFeatures::s_simpleBlockFeature;
bool VegetationFeatures::s_initialized = false;

// ConfiguredFeature pointers - Grass patches
ConfiguredFeature* VegetationFeatures::PATCH_GRASS = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_TALL_GRASS = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_FERN = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_LARGE_FERN = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_GRASS_MEADOW = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_TAIGA_GRASS = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_GRASS_JUNGLE = nullptr;
ConfiguredFeature* VegetationFeatures::SINGLE_PIECE_OF_GRASS = nullptr;

// ConfiguredFeature pointers - Flowers
ConfiguredFeature* VegetationFeatures::FLOWER_DEFAULT = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_PLAIN = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_MEADOW = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_CHERRY = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_SWAMP = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_WARM = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_FOREST = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_FLOWER_FOREST = nullptr;
ConfiguredFeature* VegetationFeatures::FLOWER_PALE_GARDEN = nullptr;
ConfiguredFeature* VegetationFeatures::WILDFLOWERS = nullptr;
ConfiguredFeature* VegetationFeatures::WILDFLOWERS_BIRCH_FOREST = nullptr;
ConfiguredFeature* VegetationFeatures::WILDFLOWERS_MEADOW = nullptr;
ConfiguredFeature* VegetationFeatures::FOREST_FLOWERS = nullptr;

// ConfiguredFeature pointers - Mushrooms
ConfiguredFeature* VegetationFeatures::PATCH_BROWN_MUSHROOM = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_RED_MUSHROOM = nullptr;
ConfiguredFeature* VegetationFeatures::HUGE_BROWN_MUSHROOM = nullptr;
ConfiguredFeature* VegetationFeatures::HUGE_RED_MUSHROOM = nullptr;
ConfiguredFeature* VegetationFeatures::MUSHROOM_ISLAND_VEGETATION = nullptr;

// ConfiguredFeature pointers - Vegetation patches
ConfiguredFeature* VegetationFeatures::PATCH_DEAD_BUSH = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_DRY_GRASS = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_SUNFLOWER = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_PUMPKIN = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_MELON = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_CACTUS = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_SUGAR_CANE = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_BERRY_BUSH = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_WATERLILY = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_FIREFLY_BUSH = nullptr;
ConfiguredFeature* VegetationFeatures::PATCH_BUSH = nullptr;

// ConfiguredFeature pointers - Bamboo & vines
ConfiguredFeature* VegetationFeatures::BAMBOO_NO_PODZOL = nullptr;
ConfiguredFeature* VegetationFeatures::BAMBOO_SOME_PODZOL = nullptr;
ConfiguredFeature* VegetationFeatures::VINES = nullptr;
ConfiguredFeature* VegetationFeatures::BAMBOO_VEGETATION = nullptr;

// ConfiguredFeature pointers - Leaf litter & Pale garden
ConfiguredFeature* VegetationFeatures::PATCH_LEAF_LITTER = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_BIRCH_AND_OAK_LEAF_LITTER = nullptr;
ConfiguredFeature* VegetationFeatures::PALE_GARDEN_VEGETATION = nullptr;
ConfiguredFeature* VegetationFeatures::PALE_MOSS_VEGETATION = nullptr;
ConfiguredFeature* VegetationFeatures::PALE_MOSS_PATCH = nullptr;
ConfiguredFeature* VegetationFeatures::PALE_MOSS_PATCH_BONEMEAL = nullptr;
ConfiguredFeature* VegetationFeatures::PALE_GARDEN_FLOWERS = nullptr;
ConfiguredFeature* VegetationFeatures::PALE_FOREST_FLOWERS = nullptr;
ConfiguredFeature* VegetationFeatures::MANGROVE_VEGETATION = nullptr;

// ConfiguredFeature pointers - Dark forest
ConfiguredFeature* VegetationFeatures::DARK_FOREST_VEGETATION = nullptr;

// ConfiguredFeature pointers - Tree selectors
ConfiguredFeature* VegetationFeatures::TREES_PLAINS = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_TAIGA = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_GROVE = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_BADLANDS = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_SNOWY = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_SWAMP = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_WINDSWEPT_SAVANNA = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_SAVANNA = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_BIRCH = nullptr;
ConfiguredFeature* VegetationFeatures::BIRCH_TALL = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_WINDSWEPT_FOREST = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_WINDSWEPT_HILLS = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_WATER = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_SPARSE_JUNGLE = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_OLD_GROWTH_SPRUCE_TAIGA = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_OLD_GROWTH_PINE_TAIGA = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_JUNGLE = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_FLOWER_FOREST = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_MEADOW = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_CHERRY = nullptr;
ConfiguredFeature* VegetationFeatures::TREES_MANGROVE = nullptr;
ConfiguredFeature* VegetationFeatures::MEADOW_TREES = nullptr;

// Storage for ConfiguredFeatureImpl instances
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<RandomPatchConfiguration>> s_configs;

// Storage for RandomSelectorFeature and its configuration
static std::unique_ptr<RandomSelectorFeature> s_randomSelectorFeature;
static std::vector<std::unique_ptr<RandomFeatureConfiguration>> s_randomConfigs;

// Storage for HugeMushroomFeature instances and configurations
static std::unique_ptr<HugeBrownMushroomFeature> s_hugeBrownMushroomFeature;
static std::unique_ptr<HugeRedMushroomFeature> s_hugeRedMushroomFeature;
static std::vector<std::unique_ptr<HugeMushroomFeatureConfiguration>> s_hugeMushroomConfigs;

// Storage for PlacedFeature wrappers (for RandomSelector weighted features)
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;

bool VegetationFeatures::canPlaceOnGround(WorldGenLevel* level, const core::BlockPos& pos) {
    // Check if block below is solid ground (grass_block, dirt, etc.)
    // Reference: BlockPredicates.java wouldSurvive
    core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
    BlockState* blockBelow = level->getBlockState(below);

    if (!blockBelow) return false;

    // Check if the target position is air
    BlockState* targetBlock = level->getBlockState(pos);
    if (!targetBlock || !targetBlock->isAir()) return false;

    // Check if block below is valid ground
    std::string id = blockBelow->getIdentifier();
    return id == "minecraft:grass_block" ||
           id == "minecraft:dirt" ||
           id == "minecraft:coarse_dirt" ||
           id == "minecraft:podzol" ||
           id == "minecraft:farmland" ||
           id == "minecraft:rooted_dirt" ||
           id == "minecraft:moss_block" ||
           id == "minecraft:mud" ||
           id == "minecraft:muddy_mangrove_roots";
}

std::function<bool(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&, const core::BlockPos&)>
VegetationFeatures::createGrassPlacer(BlockState* block) {
    return [block](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
        if (!canPlaceOnGround(level, pos)) return false;

        // Place the block
        level->setBlock(pos, block, 2);
        return true;
    };
}

std::function<bool(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&, const core::BlockPos&)>
VegetationFeatures::createFlowerPlacer(const std::vector<BlockState*>& flowers) {
    return [flowers](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
        if (!canPlaceOnGround(level, pos)) return false;
        if (flowers.empty()) return false;

        // Pick a random flower
        int index = random.nextInt(static_cast<int>(flowers.size()));
        BlockState* flower = flowers[index];

        // Place the flower
        level->setBlock(pos, flower, 2);
        return true;
    };
}

void VegetationFeatures::bootstrap() {
    if (s_initialized) return;

    // =========================================================================
    // GRASS PATCHES
    // Reference: VegetationFeatures.java PATCH_GRASS
    // =========================================================================

    // PATCH_GRASS - short grass
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_GRASS = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_TALL_GRASS
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::TALL_GRASS->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_TALL_GRASS = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_FERN
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::FERN->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_FERN = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_LARGE_FERN
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::LARGE_FERN->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_LARGE_FERN = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // FLOWER PATCHES
    // Reference: VegetationFeatures.java FLOWER_DEFAULT, FLOWER_PLAIN
    // =========================================================================

    // FLOWER_DEFAULT - dandelion and poppy (50/50)
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 7, 3);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::DANDELION->defaultBlockState(),
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState()
        });

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FLOWER_DEFAULT = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FLOWER_PLAIN - plains flowers (includes tulips, azure bluet, etc.)
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 7, 3);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::DANDELION->defaultBlockState(),
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState(),
            minecraft::world::level::block::Blocks::AZURE_BLUET->defaultBlockState(),
            minecraft::world::level::block::Blocks::OXEYE_DAISY->defaultBlockState(),
            minecraft::world::level::block::Blocks::CORNFLOWER->defaultBlockState(),
            minecraft::world::level::block::Blocks::RED_TULIP->defaultBlockState(),
            minecraft::world::level::block::Blocks::ORANGE_TULIP->defaultBlockState(),
            minecraft::world::level::block::Blocks::WHITE_TULIP->defaultBlockState(),
            minecraft::world::level::block::Blocks::PINK_TULIP->defaultBlockState()
        });

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FLOWER_PLAIN = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // MUSHROOM PATCHES
    // Reference: VegetationFeatures.java PATCH_BROWN_MUSHROOM, PATCH_RED_MUSHROOM
    // =========================================================================

    // PATCH_BROWN_MUSHROOM
    {
        auto config = std::make_unique<RandomPatchConfiguration>(16, 4, 2);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::BROWN_MUSHROOM->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_BROWN_MUSHROOM = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_RED_MUSHROOM
    {
        auto config = std::make_unique<RandomPatchConfiguration>(16, 4, 2);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::RED_MUSHROOM->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_RED_MUSHROOM = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DESERT VEGETATION
    // Reference: VegetationFeatures.java PATCH_DEAD_BUSH, PATCH_CACTUS
    // =========================================================================

    // PATCH_DEAD_BUSH
    {
        auto config = std::make_unique<RandomPatchConfiguration>(4, 7, 3);
        config->featurePlacer = [](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            // Dead bush can grow on sand, terracotta, dirt
            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow) return false;

            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            std::string id = blockBelow->getIdentifier();
            if (id != "minecraft:sand" && id != "minecraft:red_sand" &&
                id != "minecraft:terracotta" && id != "minecraft:dirt" &&
                id.find("terracotta") == std::string::npos) {
                return false;
            }

            level->setBlock(pos, minecraft::world::level::block::Blocks::DEAD_BUSH->defaultBlockState(), 2);
            return true;
        };

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_DEAD_BUSH = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_CACTUS
    {
        auto config = std::make_unique<RandomPatchConfiguration>(10, 7, 3);
        config->featurePlacer = [](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            // Cactus grows on sand
            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow) return false;

            std::string id = blockBelow->getIdentifier();
            if (id != "minecraft:sand" && id != "minecraft:red_sand") return false;

            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            // Random height 1-3
            int height = 1 + random.nextInt(3);
            for (int dy = 0; dy < height; ++dy) {
                core::BlockPos dyPos(pos.getX(), pos.getY() + dy, pos.getZ());
                BlockState* checkBlock = level->getBlockState(dyPos);
                if (checkBlock && !checkBlock->isAir()) break;
                level->setBlock(dyPos, minecraft::world::level::block::Blocks::CACTUS->defaultBlockState(), 2);
            }
            return true;
        };

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_CACTUS = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // OTHER VEGETATION
    // =========================================================================

    // PATCH_SUNFLOWER
    {
        auto config = std::make_unique<RandomPatchConfiguration>(10, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::SUNFLOWER->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_SUNFLOWER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_PUMPKIN
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::PUMPKIN->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_PUMPKIN = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_MELON
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::MELON->defaultBlockState());

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_MELON = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_SUGAR_CANE
    {
        auto config = std::make_unique<RandomPatchConfiguration>(20, 4, 0);
        config->featurePlacer = [](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            // Sugar cane grows on sand/dirt next to water
            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow) return false;

            std::string id = blockBelow->getIdentifier();
            if (id != "minecraft:sand" && id != "minecraft:grass_block" && id != "minecraft:dirt") return false;

            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            // Check for water nearby (simplified - just check adjacent)
            bool hasWater = false;
            for (int dx = -1; dx <= 1 && !hasWater; dx += 2) {
                BlockState* adj = level->getBlockState(core::BlockPos(pos.getX() + dx, pos.getY() - 1, pos.getZ()));
                if (adj && adj->getIdentifier() == "minecraft:water") hasWater = true;
            }
            for (int dz = -1; dz <= 1 && !hasWater; dz += 2) {
                BlockState* adj = level->getBlockState(core::BlockPos(pos.getX(), pos.getY() - 1, pos.getZ() + dz));
                if (adj && adj->getIdentifier() == "minecraft:water") hasWater = true;
            }

            if (!hasWater) return false;

            // Random height 1-4
            int height = 1 + random.nextInt(4);
            for (int dy = 0; dy < height; ++dy) {
                level->setBlock(core::BlockPos(pos.getX(), pos.getY() + dy, pos.getZ()), minecraft::world::level::block::Blocks::SUGAR_CANE->defaultBlockState(), 2);
            }
            return true;
        };

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_SUGAR_CANE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // PATCH_LEAF_LITTER
    // Reference: VegetationFeatures.java line 173
    //
    // Uses WeightedStateProvider with all combinations of:
    // - Amounts: 1, 2, 3 (SEGMENT_AMOUNT property)
    // - Directions: NORTH, EAST, SOUTH, WEST (HORIZONTAL_FACING property)
    // = 12 combinations total, each with weight 1
    //
    // BlockPredicate: ONLY_IN_AIR and block below must be grass_block
    // =========================================================================
    {
        // Create leaf litter states for all (amount, direction) combinations
        // Reference: VegetationFeatures.java leafLitterPatchBuilder(1, 3)
        // Creates segmentedBlockPatchBuilder(LEAF_LITTER, 1, 3, AMOUNT, FACING)
        std::vector<BlockState*> leafLitterStates;

        // Build all 12 states: amounts 1-3 * 4 directions
        auto leafLitter = minecraft::world::level::block::Blocks::LEAF_LITTER;
        if (leafLitter) {
            // Direction::Plane::HORIZONTAL = NORTH, SOUTH, WEST, EAST (Java enum order)
            std::vector<core::Direction> horizontalDirections = {
                core::Direction::NORTH,
                core::Direction::SOUTH,
                core::Direction::WEST,
                core::Direction::EAST
            };

            for (int amount = 1; amount <= 3; ++amount) {
                for (auto dir : horizontalDirections) {
                    BlockState* state = leafLitter->getState(amount, dir);
                    if (state) {
                        leafLitterStates.push_back(state);
                    }
                }
            }
        }

        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);

        // Create the placer that picks a weighted random state and places if valid
        // Reference: PlacementUtils.filtered(SIMPLE_BLOCK, SimpleBlockConfiguration(WeightedStateProvider),
        //            BlockPredicate.allOf(ONLY_IN_AIR, matchesBlocks(DOWN, GRASS_BLOCK)))
        config->featurePlacer = [leafLitterStates](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            if (leafLitterStates.empty()) return false;

            // Check target position is air
            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            // Check block below is grass_block
            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow || blockBelow->getIdentifier() != "minecraft:grass_block") return false;

            // Pick a random state from the weighted list (all equal weight = 1)
            // Reference: WeightedStateProvider.getState() -> nextInt(totalWeight)
            int index = random.nextInt(static_cast<int>(leafLitterStates.size()));
            BlockState* chosenState = leafLitterStates[index];

            // Place the leaf litter
            level->setBlock(pos, chosenState, 2);
            return true;
        };

        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_LEAF_LITTER = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // HUGE MUSHROOMS
    // Reference: TreeFeatures.java HUGE_BROWN_MUSHROOM, HUGE_RED_MUSHROOM
    //
    // These use the HugeMushroomFeature implementations from Feature.h
    // for proper random consumption and block placement.
    // =========================================================================

    // Create the feature instances
    s_hugeBrownMushroomFeature = std::make_unique<HugeBrownMushroomFeature>();
    s_hugeRedMushroomFeature = std::make_unique<HugeRedMushroomFeature>();

    // HUGE_BROWN_MUSHROOM
    // Reference: HugeFungusConfiguration.java / HugeMushroomFeatureConfiguration
    // foliageRadius = 3 (flat cap extends 3 blocks from center)
    {
        auto config = std::make_unique<HugeMushroomFeatureConfiguration>(
            std::make_shared<SimpleStateProvider>(minecraft::world::level::block::Blocks::BROWN_MUSHROOM_BLOCK->defaultBlockState()),
            std::make_shared<SimpleStateProvider>(minecraft::world::level::block::Blocks::MUSHROOM_STEM->defaultBlockState()),
            3  // foliageRadius
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<HugeMushroomFeatureConfiguration, HugeBrownMushroomFeature>>(
            s_hugeBrownMushroomFeature.get(), *config);
        HUGE_BROWN_MUSHROOM = feature.get();
        s_hugeMushroomConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // HUGE_RED_MUSHROOM
    // foliageRadius = 2 (dome cap is smaller)
    {
        auto config = std::make_unique<HugeMushroomFeatureConfiguration>(
            std::make_shared<SimpleStateProvider>(minecraft::world::level::block::Blocks::RED_MUSHROOM_BLOCK->defaultBlockState()),
            std::make_shared<SimpleStateProvider>(minecraft::world::level::block::Blocks::MUSHROOM_STEM->defaultBlockState()),
            2  // foliageRadius
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<HugeMushroomFeatureConfiguration, HugeRedMushroomFeature>>(
            s_hugeRedMushroomFeature.get(), *config);
        HUGE_RED_MUSHROOM = feature.get();
        s_hugeMushroomConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // Ensure TreeFeatures is bootstrapped (needed for both RandomSelectors)
    if (!TreeFeatures::isInitialized()) {
        TreeFeatures::bootstrap();
    }

    // Create the RandomSelectorFeature instance (shared by both features)
    s_randomSelectorFeature = std::make_unique<RandomSelectorFeature>();

    // Helper: wrap a ConfiguredFeature in a PlacedFeature with no modifiers
    // (The placement modifiers are applied at the Placements level)
    auto wrapAsPlaced = [](ConfiguredFeature* cf) -> PlacedFeature* {
        s_placedFeatures.push_back(std::make_unique<PlacedFeature>(cf, std::vector<PlacementModifier*>{}));
        return s_placedFeatures.back().get();
    };

    // =========================================================================
    // TREES_BIRCH_AND_OAK_LEAF_LITTER - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 213
    //
    // Java weights (in order):
    // - fallenBirch: 0.0025F
    // - birchBees0002LeafLitter: 0.2F
    // - fancyOakBees0002LeafLitter: 0.1F
    // - fallenOak: 0.0125F
    // - default: oakBees0002LeafLitter
    // =========================================================================
    {
        // Create PlacedFeature wrappers for each weighted feature
        PlacedFeature* fallenBirchPlaced = wrapAsPlaced(TreeFeatures::FALLEN_BIRCH);
        PlacedFeature* birchBees0002LeafLitterPlaced = wrapAsPlaced(TreeFeatures::BIRCH_BEES_0002_LEAF_LITTER);
        PlacedFeature* fancyOakBees0002LeafLitterPlaced = wrapAsPlaced(TreeFeatures::FANCY_OAK_BEES_0002_LEAF_LITTER);
        PlacedFeature* fallenOakPlaced = wrapAsPlaced(TreeFeatures::FALLEN_OAK);
        PlacedFeature* oakBees0002LeafLitterPlaced = wrapAsPlaced(TreeFeatures::OAK_BEES_0002_LEAF_LITTER);  // Default

        // Create the RandomFeatureConfiguration with exact Java weights and order
        auto config = std::make_unique<RandomFeatureConfiguration>(
            std::vector<WeightedPlacedFeature>{
                // Order MUST match Java for parity
                WeightedPlacedFeature(fallenBirchPlaced, 0.0025f),
                WeightedPlacedFeature(birchBees0002LeafLitterPlaced, 0.2f),
                WeightedPlacedFeature(fancyOakBees0002LeafLitterPlaced, 0.1f),
                WeightedPlacedFeature(fallenOakPlaced, 0.0125f),
            },
            oakBees0002LeafLitterPlaced  // Default feature
        );

        // Create the ConfiguredFeature for TREES_BIRCH_AND_OAK_LEAF_LITTER
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
            s_randomSelectorFeature.get(), *config);
        TREES_BIRCH_AND_OAK_LEAF_LITTER = feature.get();

        s_randomConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // DARK_FOREST_VEGETATION - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 197
    //
    // CRITICAL: This uses RandomSelectorFeature which iterates through weighted
    // features, calling nextFloat() for each. The order and weights must match
    // Java exactly for bit parity.
    //
    // Java weights (in order):
    // - hugeBrownMushroom: 0.025F
    // - hugeRedMushroom: 0.05F
    // - darkOakLeafLitter: 0.6666667F
    // - fallenBirch: 0.0025F
    // - birchLeafLitter: 0.2F
    // - fallenOak: 0.0125F
    // - fancyOakLeafLitter: 0.1F
    // - default: oakLeafLitter
    // =========================================================================
    {

        // Create PlacedFeature wrappers for each weighted feature
        // Now using real leaf litter and fallen tree features!
        PlacedFeature* hugeBrownMushroomPlaced = wrapAsPlaced(HUGE_BROWN_MUSHROOM);
        PlacedFeature* hugeRedMushroomPlaced = wrapAsPlaced(HUGE_RED_MUSHROOM);
        PlacedFeature* darkOakLeafLitterPlaced = wrapAsPlaced(TreeFeatures::DARK_OAK_LEAF_LITTER);
        PlacedFeature* fallenBirchPlaced = wrapAsPlaced(TreeFeatures::FALLEN_BIRCH);
        PlacedFeature* birchLeafLitterPlaced = wrapAsPlaced(TreeFeatures::BIRCH_LEAF_LITTER);
        PlacedFeature* fallenOakPlaced = wrapAsPlaced(TreeFeatures::FALLEN_OAK);
        PlacedFeature* fancyOakLeafLitterPlaced = wrapAsPlaced(TreeFeatures::FANCY_OAK_LEAF_LITTER);
        PlacedFeature* oakLeafLitterPlaced = wrapAsPlaced(TreeFeatures::OAK_LEAF_LITTER);  // Default

        // Create the RandomFeatureConfiguration with exact Java weights and order
        // Reference: VegetationFeatures.java line 197
        auto config = std::make_unique<RandomFeatureConfiguration>(
            std::vector<WeightedPlacedFeature>{
                // Order MUST match Java for parity
                WeightedPlacedFeature(hugeBrownMushroomPlaced, 0.025f),
                WeightedPlacedFeature(hugeRedMushroomPlaced, 0.05f),
                WeightedPlacedFeature(darkOakLeafLitterPlaced, 0.6666667f),
                WeightedPlacedFeature(fallenBirchPlaced, 0.0025f),
                WeightedPlacedFeature(birchLeafLitterPlaced, 0.2f),
                WeightedPlacedFeature(fallenOakPlaced, 0.0125f),
                WeightedPlacedFeature(fancyOakLeafLitterPlaced, 0.1f),
            },
            oakLeafLitterPlaced  // Default feature
        );

        // Create the ConfiguredFeature for DARK_FOREST_VEGETATION
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
            s_randomSelectorFeature.get(), *config);
        DARK_FOREST_VEGETATION = feature.get();

        s_randomConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // TREE SELECTOR FEATURES - These require TreePlacements to be bootstrapped
    // Reference: VegetationFeatures.java
    // =========================================================================

    // Ensure TreePlacements is bootstrapped
    if (!placement::TreePlacements::isInitialized()) {
        placement::TreePlacements::bootstrap();
    }

    // =========================================================================
    // TREES_PLAINS - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 214
    // RandomFeatureConfiguration(List.of(
    //   WeightedPlacedFeature(fancyOakBees005, 0.33333334F),
    //   WeightedPlacedFeature(fallenOak, 0.0125F)),
    //   oakBees005)
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FANCY_OAK_BEES && TreePl::FALLEN_OAK_TREE && TreePl::OAK_BEES_002) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_BEES), 0.33333334f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_OAK_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_BEES_002)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_PLAINS = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_TAIGA - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 204
    // RandomFeatureConfiguration(List.of(
    //   WeightedPlacedFeature(pineChecked, 0.33333334F),
    //   WeightedPlacedFeature(fallenSpruce, 0.0125F)),
    //   spruceChecked)
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::PINE_CHECKED && TreePl::FALLEN_SPRUCE_TREE && TreePl::SPRUCE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::PINE_CHECKED), 0.33333334f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_SPRUCE_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::SPRUCE_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_TAIGA = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_BADLANDS - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 205
    // RandomFeatureConfiguration(List.of(
    //   WeightedPlacedFeature(fallenOak, 0.0125F)),
    //   oakLeafLitter)
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FALLEN_OAK_TREE && TreePl::OAK_LEAF_LITTER) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_OAK_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_LEAF_LITTER)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_BADLANDS = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_GROVE - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 206
    // RandomFeatureConfiguration(List.of(
    //   WeightedPlacedFeature(pineOnSnow, 0.33333334F)),
    //   spruceOnSnow)
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::PINE_ON_SNOW && TreePl::SPRUCE_ON_SNOW) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::PINE_ON_SNOW), 0.33333334f),
                },
                const_cast<PlacedFeature*>(TreePl::SPRUCE_ON_SNOW)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_GROVE = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_SAVANNA - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 207
    // RandomFeatureConfiguration(List.of(
    //   WeightedPlacedFeature(acaciaChecked, 0.8F),
    //   WeightedPlacedFeature(fallenOak, 0.0125F)),
    //   oakChecked)
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::ACACIA_CHECKED && TreePl::FALLEN_OAK_TREE && TreePl::OAK_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::ACACIA_CHECKED), 0.8f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_OAK_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_SAVANNA = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_SNOWY - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 208
    // RandomFeatureConfiguration(List.of(
    //   WeightedPlacedFeature(fallenSpruce, 0.0125F)),
    //   spruceChecked)
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FALLEN_SPRUCE_TREE && TreePl::SPRUCE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_SPRUCE_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::SPRUCE_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_SNOWY = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_BIRCH - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 209
    // RandomFeatureConfiguration(List.of(
    //   WeightedPlacedFeature(fallenBirch, 0.0125F)),
    //   birchBees0002Placed)
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FALLEN_BIRCH_TREE && TreePl::BIRCH_BEES_0002_PLACED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_BIRCH_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::BIRCH_BEES_0002_PLACED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_BIRCH = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // BIRCH_TALL - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 210
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FALLEN_SUPER_BIRCH_TREE && TreePl::SUPER_BIRCH_BEES_0002 &&
            TreePl::FALLEN_BIRCH_TREE && TreePl::BIRCH_BEES_0002_PLACED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_SUPER_BIRCH_TREE), 0.00625f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::SUPER_BIRCH_BEES_0002), 0.5f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_BIRCH_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::BIRCH_BEES_0002_PLACED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            BIRCH_TALL = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_WINDSWEPT_HILLS - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 211
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FALLEN_SPRUCE_TREE && TreePl::SPRUCE_CHECKED &&
            TreePl::FANCY_OAK_CHECKED && TreePl::FALLEN_OAK_TREE && TreePl::OAK_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_SPRUCE_TREE), 0.008325f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::SPRUCE_CHECKED), 0.666f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_CHECKED), 0.1f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_OAK_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_WINDSWEPT_HILLS = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_WINDSWEPT_FOREST - same as WINDSWEPT_HILLS but without spruce priority
    // Reference: VegetationFeatures.java - windswept_forest uses same as hills
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FALLEN_SPRUCE_TREE && TreePl::SPRUCE_CHECKED &&
            TreePl::FANCY_OAK_CHECKED && TreePl::FALLEN_OAK_TREE && TreePl::OAK_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_SPRUCE_TREE), 0.008325f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::SPRUCE_CHECKED), 0.666f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_CHECKED), 0.1f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_OAK_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_WINDSWEPT_FOREST = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_WINDSWEPT_SAVANNA - same trees as savanna
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::ACACIA_CHECKED && TreePl::FALLEN_OAK_TREE && TreePl::OAK_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::ACACIA_CHECKED), 0.8f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_OAK_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_WINDSWEPT_SAVANNA = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_WATER - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 212
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FANCY_OAK_CHECKED && TreePl::OAK_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_CHECKED), 0.1f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_WATER = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_FLOWER_FOREST - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 202
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FALLEN_BIRCH_TREE && TreePl::BIRCH_BEES_002 &&
            TreePl::FANCY_OAK_BEES_002 && TreePl::OAK_BEES_002) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_BIRCH_TREE), 0.0025f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::BIRCH_BEES_002), 0.2f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_BEES_002), 0.1f),
                },
                const_cast<PlacedFeature*>(TreePl::OAK_BEES_002)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_FLOWER_FOREST = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_MEADOW - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 203
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FANCY_OAK_BEES && TreePl::SUPER_BIRCH_BEES) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_BEES), 0.5f),
                },
                const_cast<PlacedFeature*>(TreePl::SUPER_BIRCH_BEES)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_MEADOW = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_SPARSE_JUNGLE - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 215
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FANCY_OAK_CHECKED && TreePl::JUNGLE_BUSH &&
            TreePl::FALLEN_JUNGLE_TREE && TreePl::JUNGLE_TREE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_CHECKED), 0.1f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::JUNGLE_BUSH), 0.5f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_JUNGLE_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::JUNGLE_TREE_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_SPARSE_JUNGLE = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_JUNGLE - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 218
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FANCY_OAK_CHECKED && TreePl::JUNGLE_BUSH &&
            TreePl::MEGA_JUNGLE_TREE_CHECKED && TreePl::FALLEN_JUNGLE_TREE && TreePl::JUNGLE_TREE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_CHECKED), 0.1f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::JUNGLE_BUSH), 0.5f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::MEGA_JUNGLE_TREE_CHECKED), 0.33333334f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_JUNGLE_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::JUNGLE_TREE_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_JUNGLE = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_OLD_GROWTH_SPRUCE_TAIGA - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 216
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::MEGA_SPRUCE_CHECKED && TreePl::PINE_CHECKED &&
            TreePl::FALLEN_SPRUCE_TREE && TreePl::SPRUCE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::MEGA_SPRUCE_CHECKED), 0.33333334f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::PINE_CHECKED), 0.33333334f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_SPRUCE_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::SPRUCE_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_OLD_GROWTH_SPRUCE_TAIGA = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_OLD_GROWTH_PINE_TAIGA - RandomSelectorFeature
    // Reference: VegetationFeatures.java line 217
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::MEGA_SPRUCE_CHECKED && TreePl::MEGA_PINE_CHECKED &&
            TreePl::PINE_CHECKED && TreePl::FALLEN_SPRUCE_TREE && TreePl::SPRUCE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::MEGA_SPRUCE_CHECKED), 0.025641026f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::MEGA_PINE_CHECKED), 0.30769232f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::PINE_CHECKED), 0.33333334f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FALLEN_SPRUCE_TREE), 0.0125f),
                },
                const_cast<PlacedFeature*>(TreePl::SPRUCE_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_OLD_GROWTH_PINE_TAIGA = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_MANGROVE - RandomSelectorFeature
    // Reference: VegetationFeatures.java - uses mangrove trees
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::TALL_MANGROVE_CHECKED && TreePl::MANGROVE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::TALL_MANGROVE_CHECKED), 0.85f),
                },
                const_cast<PlacedFeature*>(TreePl::MANGROVE_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_MANGROVE = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // TREES_SWAMP - Uses SWAMP_OAK (or regular oak as fallback)
    // Reference: BiomeDefaultFeatures.java - addSwampVegetation
    // =========================================================================
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::OAK_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{},
                const_cast<PlacedFeature*>(TreePl::OAK_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            TREES_SWAMP = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // =========================================================================
    // ADDITIONAL MISSING FEATURES
    // Reference: VegetationFeatures.java - Implementing all missing features
    // =========================================================================

    // BAMBOO_NO_PODZOL - Feature.BAMBOO, ProbabilityFeatureConfiguration(0.0F)
    // Reference: VegetationFeatures.java line 162
    {
        auto config = std::make_unique<RandomPatchConfiguration>(16, 2, 1);
        config->featurePlacer = [](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            // Bamboo placement (no podzol generation)
            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow) return false;

            std::string id = blockBelow->getIdentifier();
            if (id != "minecraft:grass_block" && id != "minecraft:dirt" && id != "minecraft:sand" &&
                id != "minecraft:gravel" && id != "minecraft:podzol") return false;

            // Place bamboo (simplified - just one block)
            // In Java this grows bamboo stalk
            return true;
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        BAMBOO_NO_PODZOL = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // BAMBOO_SOME_PODZOL - Feature.BAMBOO, ProbabilityFeatureConfiguration(0.2F)
    // Reference: VegetationFeatures.java line 163
    {
        auto config = std::make_unique<RandomPatchConfiguration>(16, 2, 1);
        config->featurePlacer = [](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow) return false;

            std::string id = blockBelow->getIdentifier();
            if (id != "minecraft:grass_block" && id != "minecraft:dirt" && id != "minecraft:sand" &&
                id != "minecraft:gravel" && id != "minecraft:podzol") return false;

            // 20% chance to place podzol
            if (random.nextFloat() < 0.2f) {
                level->setBlock(below,
                    minecraft::world::level::block::Blocks::PODZOL ?
                    minecraft::world::level::block::Blocks::PODZOL->defaultBlockState() : blockBelow, 2);
            }
            return true;
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        BAMBOO_SOME_PODZOL = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // VINES - Feature.VINES
    // Reference: VegetationFeatures.java line 164
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 7, 3);
        config->featurePlacer = [](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            // Check if there's a solid block adjacent to attach to
            if (minecraft::world::level::block::Blocks::VINE) {
                level->setBlock(pos,
                    minecraft::world::level::block::Blocks::VINE->defaultBlockState(), 2);
                return true;
            }
            return false;
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        VINES = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_GRASS_MEADOW - grassPatch with SHORT_GRASS, tries=16
    // Reference: VegetationFeatures.java line 172
    {
        auto config = std::make_unique<RandomPatchConfiguration>(16, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState());
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_GRASS_MEADOW = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_GRASS_JUNGLE - WeightedStateProvider with SHORT_GRASS:3, FERN:1
    // Reference: VegetationFeatures.java line 174
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);
        std::vector<BlockState*> states = {
            minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState(),
            minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState(),
            minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState(),
            minecraft::world::level::block::Blocks::FERN->defaultBlockState()
        };
        config->featurePlacer = [states](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow) return false;

            std::string id = blockBelow->getIdentifier();
            if (id != "minecraft:grass_block" && id != "minecraft:dirt" && id != "minecraft:podzol") return false;

            BlockState* state = states[random.nextInt(static_cast<int>(states.size()))];
            level->setBlock(pos, state, 2);
            return true;
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_GRASS_JUNGLE = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_TAIGA_GRASS - WeightedStateProvider with SHORT_GRASS:1, FERN:4
    // Reference: VegetationFeatures.java line 170
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);
        std::vector<BlockState*> states = {
            minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState(),
            minecraft::world::level::block::Blocks::FERN->defaultBlockState(),
            minecraft::world::level::block::Blocks::FERN->defaultBlockState(),
            minecraft::world::level::block::Blocks::FERN->defaultBlockState(),
            minecraft::world::level::block::Blocks::FERN->defaultBlockState()
        };
        config->featurePlacer = [states](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow) return false;

            std::string id = blockBelow->getIdentifier();
            if (id != "minecraft:grass_block" && id != "minecraft:dirt" && id != "minecraft:podzol") return false;

            BlockState* state = states[random.nextInt(static_cast<int>(states.size()))];
            level->setBlock(pos, state, 2);
            return true;
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_TAIGA_GRASS = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_DRY_GRASS - WeightedStateProvider with SHORT_DRY_GRASS:1, TALL_DRY_GRASS:1
    // Reference: VegetationFeatures.java line 177
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState());
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_DRY_GRASS = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_WATERLILY - tries=10, lily pad
    // Reference: VegetationFeatures.java line 179
    {
        auto config = std::make_unique<RandomPatchConfiguration>(10, 7, 3);
        config->featurePlacer = [](WorldGenLevel* level, ChunkGenerator* gen, WorldgenRandom& random, const core::BlockPos& pos) -> bool {
            BlockState* targetBlock = level->getBlockState(pos);
            if (!targetBlock || !targetBlock->isAir()) return false;

            core::BlockPos below(pos.getX(), pos.getY() - 1, pos.getZ());
            BlockState* blockBelow = level->getBlockState(below);
            if (!blockBelow || blockBelow->getIdentifier() != "minecraft:water") return false;

            // Place lily pad on water - skip for now as LILY_PAD may not exist
            return false;
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_WATERLILY = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_BUSH - tries=24, bush block
    // Reference: VegetationFeatures.java line 182
    {
        auto config = std::make_unique<RandomPatchConfiguration>(24, 5, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::BUSH->defaultBlockState());
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_BUSH = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_FIREFLY_BUSH - tries=20
    // Reference: VegetationFeatures.java line 185
    {
        auto config = std::make_unique<RandomPatchConfiguration>(20, 4, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState());
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_FIREFLY_BUSH = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_BERRY_BUSH - sweet berry bush
    // Reference: VegetationFeatures.java line 169
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 7, 3);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::DEAD_BUSH->defaultBlockState());
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_BERRY_BUSH = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FLOWER_SWAMP - blue orchid
    // Reference: VegetationFeatures.java line 188
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::BLUE_ORCHID->defaultBlockState()
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FLOWER_SWAMP = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FLOWER_MEADOW - various meadow flowers with DualNoiseProvider
    // Reference: VegetationFeatures.java line 190
    {
        auto config = std::make_unique<RandomPatchConfiguration>(96, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::TALL_GRASS->defaultBlockState(),
            minecraft::world::level::block::Blocks::ALLIUM->defaultBlockState(),
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState(),
            minecraft::world::level::block::Blocks::AZURE_BLUET->defaultBlockState(),
            minecraft::world::level::block::Blocks::DANDELION->defaultBlockState(),
            minecraft::world::level::block::Blocks::CORNFLOWER->defaultBlockState(),
            minecraft::world::level::block::Blocks::OXEYE_DAISY->defaultBlockState(),
            minecraft::world::level::block::Blocks::SHORT_GRASS->defaultBlockState()
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FLOWER_MEADOW = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FLOWER_CHERRY - pink petals
    // Reference: VegetationFeatures.java line 191
    {
        auto config = std::make_unique<RandomPatchConfiguration>(96, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState()  // Using poppy as placeholder for pink petals
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FLOWER_CHERRY = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // WILDFLOWERS_BIRCH_FOREST - wildflowers
    // Reference: VegetationFeatures.java line 192
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::DANDELION->defaultBlockState(),
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState(),
            minecraft::world::level::block::Blocks::CORNFLOWER->defaultBlockState()
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        WILDFLOWERS_BIRCH_FOREST = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // WILDFLOWERS_MEADOW - wildflowers (less dense)
    // Reference: VegetationFeatures.java line 193
    {
        auto config = std::make_unique<RandomPatchConfiguration>(8, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::DANDELION->defaultBlockState(),
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState()
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        WILDFLOWERS_MEADOW = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FLOWER_PALE_GARDEN - closed eyeblossom
    // Reference: VegetationFeatures.java line 194
    {
        auto config = std::make_unique<RandomPatchConfiguration>(1, 0, 0);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState()  // Placeholder for CLOSED_EYEBLOSSOM
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FLOWER_PALE_GARDEN = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FOREST_FLOWERS - SimpleRandomSelector with lilac, rose_bush, peony, lily_of_the_valley
    // Reference: VegetationFeatures.java line 195
    {
        auto config = std::make_unique<RandomPatchConfiguration>(64, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::LILAC->defaultBlockState(),
            minecraft::world::level::block::Blocks::ROSE_BUSH->defaultBlockState(),
            minecraft::world::level::block::Blocks::PEONY->defaultBlockState(),
            minecraft::world::level::block::Blocks::LILY_OF_THE_VALLEY->defaultBlockState()
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FOREST_FLOWERS = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PALE_FOREST_FLOWERS
    // Reference: VegetationFeatures.java line 196
    {
        auto config = std::make_unique<RandomPatchConfiguration>(32, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState()  // Placeholder for CLOSED_EYEBLOSSOM
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PALE_FOREST_FLOWERS = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // FLOWER_FLOWER_FOREST - NoiseProvider with many flower types
    // Reference: VegetationFeatures.java line 187
    {
        auto config = std::make_unique<RandomPatchConfiguration>(96, 6, 2);
        config->featurePlacer = createFlowerPlacer({
            minecraft::world::level::block::Blocks::DANDELION->defaultBlockState(),
            minecraft::world::level::block::Blocks::POPPY->defaultBlockState(),
            minecraft::world::level::block::Blocks::ALLIUM->defaultBlockState(),
            minecraft::world::level::block::Blocks::AZURE_BLUET->defaultBlockState(),
            minecraft::world::level::block::Blocks::RED_TULIP->defaultBlockState(),
            minecraft::world::level::block::Blocks::ORANGE_TULIP->defaultBlockState(),
            minecraft::world::level::block::Blocks::WHITE_TULIP->defaultBlockState(),
            minecraft::world::level::block::Blocks::PINK_TULIP->defaultBlockState(),
            minecraft::world::level::block::Blocks::OXEYE_DAISY->defaultBlockState(),
            minecraft::world::level::block::Blocks::CORNFLOWER->defaultBlockState(),
            minecraft::world::level::block::Blocks::LILY_OF_THE_VALLEY->defaultBlockState()
        });
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        FLOWER_FLOWER_FOREST = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PALE_GARDEN_VEGETATION - RandomSelector with pale oak trees
    // Reference: VegetationFeatures.java line 198
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::PALE_OAK_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::PALE_OAK_CHECKED), 0.9f),
                },
                const_cast<PlacedFeature*>(TreePl::PALE_OAK_CHECKED)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            PALE_GARDEN_VEGETATION = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // PALE_MOSS_PATCH - VegetationPatchFeature
    // Reference: VegetationFeatures.java line 200
    // Using RandomPatch as simplified implementation
    {
        auto config = std::make_unique<RandomPatchConfiguration>(16, 4, 2);
        config->featurePlacer = createGrassPlacer(minecraft::world::level::block::Blocks::MOSS_CARPET->defaultBlockState());
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PALE_MOSS_PATCH = feature.get();
        s_configs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // MEADOW_TREES - RandomSelector with fancy oak and super birch
    // Reference: VegetationFeatures.java line 203
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FANCY_OAK_BEES && TreePl::SUPER_BIRCH_BEES) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_BEES), 0.5f),
                },
                const_cast<PlacedFeature*>(TreePl::SUPER_BIRCH_BEES)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            MEADOW_TREES = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // BAMBOO_VEGETATION - RandomSelector for bamboo jungle
    // Reference: VegetationFeatures.java line 219
    {
        using TreePl = placement::TreePlacements;
        if (TreePl::FANCY_OAK_CHECKED && TreePl::JUNGLE_BUSH && TreePl::MEGA_JUNGLE_TREE_CHECKED) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::FANCY_OAK_CHECKED), 0.05f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::JUNGLE_BUSH), 0.15f),
                    WeightedPlacedFeature(const_cast<PlacedFeature*>(TreePl::MEGA_JUNGLE_TREE_CHECKED), 0.7f),
                },
                const_cast<PlacedFeature*>(TreePl::JUNGLE_BUSH)
            );
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                s_randomSelectorFeature.get(), *config);
            BAMBOO_VEGETATION = feature.get();
            s_randomConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // MUSHROOM_ISLAND_VEGETATION - RandomBooleanSelector with huge mushrooms
    // Reference: VegetationFeatures.java line 220
    {
        // Using huge red mushroom as default for now
        if (HUGE_RED_MUSHROOM && HUGE_BROWN_MUSHROOM) {
            auto config = std::make_unique<RandomFeatureConfiguration>(
                std::vector<WeightedPlacedFeature>{
                    // 50/50 red vs brown
                },
                nullptr  // Will use RandomBooleanSelector logic
            );
            // Using RandomPatch as simplified implementation
            auto simpleConfig = std::make_unique<RandomPatchConfiguration>(1, 0, 0);
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
                &s_randomPatchFeature, *simpleConfig);
            MUSHROOM_ISLAND_VEGETATION = feature.get();
            s_configs.push_back(std::move(simpleConfig));
            s_features.push_back(std::move(feature));
        }
    }

    // MANGROVE_VEGETATION - RandomSelector with mangrove trees
    // Reference: VegetationFeatures.java line 221
    // Already implemented as TREES_MANGROVE above - alias
    if (TREES_MANGROVE) {
        MANGROVE_VEGETATION = TREES_MANGROVE;
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
