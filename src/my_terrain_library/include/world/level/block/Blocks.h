#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/blocks/StairBlock.h"
#include "world/level/block/blocks/SlabBlock.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/blocks/DoorBlock.h"
#include "world/level/block/blocks/WallBlock.h"
#include "world/level/block/blocks/LeavesBlock.h"
#include "world/level/block/blocks/LeafLitterBlock.h"
#include "world/level/block/blocks/RotatedPillarBlock.h"
#include "world/level/block/blocks/SculkVeinBlock.h"
#include <memory>
#include <unordered_map>
#include <string>

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;

/**
 * Blocks - Registry of all block types
 * Reference: net/minecraft/world/level/block/Blocks.java
 *
 * Provides access to all registered blocks and their default states.
 * Blocks are registered during initialization via bootstrap().
 */
class Blocks {
public:
    // =========================================================================
    // Basic blocks (no properties)
    // =========================================================================
    static Block* AIR;
    static Block* CAVE_AIR;
    static Block* STONE;
    static Block* GRANITE;
    static Block* DIORITE;
    static Block* ANDESITE;
    static Block* DEEPSLATE;
    static Block* COBBLESTONE;
    static Block* MOSSY_COBBLESTONE;
    static Block* DIRT;
    static Block* COARSE_DIRT;
    static Block* PODZOL;
    static Block* GRASS_BLOCK;
    static Block* SAND;
    static Block* GRAVEL;
    static Block* BEDROCK;
    static Block* WATER;
    static Block* LAVA;
    static Block* TUFF;
    static Block* DRIPSTONE_BLOCK;
    static Block* POINTED_DRIPSTONE;
    static Block* SANDSTONE;

    // =========================================================================
    // Ice and snow blocks
    // =========================================================================
    static Block* SNOW_BLOCK;
    static Block* PACKED_ICE;
    static Block* BLUE_ICE;
    static Block* ICE;
    static Block* POWDER_SNOW;
    static Block* SNOW;  // Snow layer block (1-8 layers)

    // =========================================================================
    // Geode blocks
    // Reference: Used in amethyst geode feature
    // =========================================================================
    static Block* AMETHYST_BLOCK;
    static Block* BUDDING_AMETHYST;
    static Block* CALCITE;
    static Block* SMOOTH_BASALT;
    static Block* SMALL_AMETHYST_BUD;
    static Block* MEDIUM_AMETHYST_BUD;
    static Block* LARGE_AMETHYST_BUD;
    static Block* AMETHYST_CLUSTER;

    // =========================================================================
    // Clay and mud blocks
    // =========================================================================
    static Block* CLAY;
    static Block* MUD;
    static Block* MAGMA_BLOCK;

    // =========================================================================
    // Sculk blocks (deep dark biome)
    // Reference: Used in sculk patch feature
    // =========================================================================
    static Block* SCULK;
    static Block* SCULK_CATALYST;
    static Block* SCULK_SENSOR;
    static Block* SCULK_SHRIEKER;
    static SculkVeinBlock* SCULK_VEIN;

    // =========================================================================
    // Ore blocks
    // =========================================================================
    static Block* COPPER_ORE;
    static Block* DEEPSLATE_COPPER_ORE;
    static Block* IRON_ORE;
    static Block* DEEPSLATE_IRON_ORE;
    static Block* COAL_ORE;
    static Block* DEEPSLATE_COAL_ORE;
    static Block* GOLD_ORE;
    static Block* DEEPSLATE_GOLD_ORE;
    static Block* DIAMOND_ORE;
    static Block* DEEPSLATE_DIAMOND_ORE;
    static Block* REDSTONE_ORE;
    static Block* DEEPSLATE_REDSTONE_ORE;
    static Block* LAPIS_ORE;
    static Block* DEEPSLATE_LAPIS_ORE;
    static Block* EMERALD_ORE;
    static Block* DEEPSLATE_EMERALD_ORE;

    // =========================================================================
    // Raw ore blocks
    // =========================================================================
    static Block* RAW_COPPER_BLOCK;
    static Block* RAW_IRON_BLOCK;

    // =========================================================================
    // Infested blocks
    // Reference: Used by silverfish spawning and ore infested feature
    // =========================================================================
    static Block* INFESTED_STONE;
    static Block* INFESTED_DEEPSLATE;

    // =========================================================================
    // Terracotta blocks
    // =========================================================================
    static Block* TERRACOTTA;
    static Block* WHITE_TERRACOTTA;
    static Block* ORANGE_TERRACOTTA;
    static Block* YELLOW_TERRACOTTA;
    static Block* BROWN_TERRACOTTA;
    static Block* RED_TERRACOTTA;
    static Block* LIGHT_GRAY_TERRACOTTA;

    // =========================================================================
    // Vegetation - Small plants (no collision)
    // =========================================================================
    static Block* SHORT_GRASS;
    static Block* TALL_GRASS;
    static Block* FERN;
    static Block* LARGE_FERN;
    static Block* DEAD_BUSH;
    static Block* BUSH;

    // =========================================================================
    // Flowers (no collision)
    // =========================================================================
    static Block* DANDELION;
    static Block* POPPY;
    static Block* BLUE_ORCHID;
    static Block* ALLIUM;
    static Block* AZURE_BLUET;
    static Block* RED_TULIP;
    static Block* ORANGE_TULIP;
    static Block* WHITE_TULIP;
    static Block* PINK_TULIP;
    static Block* OXEYE_DAISY;
    static Block* CORNFLOWER;
    static Block* LILY_OF_THE_VALLEY;

    // =========================================================================
    // Tall flowers (two-block, no collision)
    // =========================================================================
    static Block* SUNFLOWER;
    static Block* LILAC;
    static Block* ROSE_BUSH;
    static Block* PEONY;

    // =========================================================================
    // Mushrooms (small, no collision)
    // =========================================================================
    static Block* BROWN_MUSHROOM;
    static Block* RED_MUSHROOM;

    // =========================================================================
    // Huge mushroom blocks (solid)
    // Note: These have directional properties (NORTH, EAST, SOUTH, WEST, UP, DOWN)
    // For now, using basic Block until HugeMushroomBlock is implemented
    // =========================================================================
    static Block* BROWN_MUSHROOM_BLOCK;
    static Block* RED_MUSHROOM_BLOCK;
    static Block* MUSHROOM_STEM;

    // =========================================================================
    // Leaf litter and vines
    // =========================================================================
    static LeafLitterBlock* LEAF_LITTER;
    static Block* VINE;

    // =========================================================================
    // Moss and lush cave vegetation
    // Reference: Used in lush caves biome features
    // =========================================================================
    static Block* MOSS_BLOCK;
    static Block* MOSS_CARPET;
    static Block* CAVE_VINES;
    static Block* CAVE_VINES_PLANT;
    static Block* GLOW_LICHEN;
    static Block* AZALEA;
    static Block* FLOWERING_AZALEA;
    static Block* SPORE_BLOSSOM;
    static Block* BIG_DRIPLEAF;
    static Block* BIG_DRIPLEAF_STEM;
    static Block* SMALL_DRIPLEAF;

    // =========================================================================
    // Pale garden vegetation
    // Reference: Used in pale garden biome features
    // =========================================================================
    static Block* PALE_MOSS_BLOCK;
    static Block* PALE_MOSS_CARPET;
    static Block* PALE_HANGING_MOSS;

    // =========================================================================
    // Ocean vegetation
    // Reference: Used in ocean biome features
    // =========================================================================
    static Block* SEAGRASS;
    static Block* TALL_SEAGRASS;
    static Block* KELP;
    static Block* KELP_PLANT;

    // =========================================================================
    // Other vegetation
    // =========================================================================
    static Block* CACTUS;
    static Block* SUGAR_CANE;
    static Block* PUMPKIN;
    static Block* MELON;

    // =========================================================================
    // Dungeon blocks
    // Reference: Used in MonsterRoomFeature
    // =========================================================================
    static Block* SPAWNER;
    static Block* CHEST;

    // =========================================================================
    // Blocks with properties - Stairs
    // =========================================================================
    static StairBlock* OAK_STAIRS;
    static StairBlock* STONE_STAIRS;
    static StairBlock* COBBLESTONE_STAIRS;

    // =========================================================================
    // Blocks with properties - Slabs
    // =========================================================================
    static SlabBlock* OAK_SLAB;
    static SlabBlock* STONE_SLAB;
    static SlabBlock* COBBLESTONE_SLAB;

    // =========================================================================
    // Blocks with properties - Fences
    // =========================================================================
    static FenceBlock* OAK_FENCE;
    static FenceBlock* NETHER_BRICK_FENCE;

    // =========================================================================
    // Blocks with properties - Doors
    // =========================================================================
    static DoorBlock* OAK_DOOR;
    static DoorBlock* IRON_DOOR;

    // =========================================================================
    // Blocks with properties - Walls
    // =========================================================================
    static WallBlock* COBBLESTONE_WALL;
    static WallBlock* STONE_BRICK_WALL;

    // =========================================================================
    // Blocks with properties - Leaves
    // =========================================================================
    static LeavesBlock* OAK_LEAVES;
    static LeavesBlock* SPRUCE_LEAVES;
    static LeavesBlock* BIRCH_LEAVES;
    static LeavesBlock* JUNGLE_LEAVES;
    static LeavesBlock* ACACIA_LEAVES;
    static LeavesBlock* DARK_OAK_LEAVES;
    static LeavesBlock* AZALEA_LEAVES;
    static LeavesBlock* FLOWERING_AZALEA_LEAVES;
    static LeavesBlock* MANGROVE_LEAVES;
    static LeavesBlock* CHERRY_LEAVES;
    static LeavesBlock* PALE_OAK_LEAVES;

    // =========================================================================
    // Blocks with properties - Logs (RotatedPillarBlock with AXIS)
    // =========================================================================
    static RotatedPillarBlock* OAK_LOG;
    static RotatedPillarBlock* SPRUCE_LOG;
    static RotatedPillarBlock* BIRCH_LOG;
    static RotatedPillarBlock* JUNGLE_LOG;
    static RotatedPillarBlock* ACACIA_LOG;
    static RotatedPillarBlock* DARK_OAK_LOG;
    static RotatedPillarBlock* MANGROVE_LOG;
    static RotatedPillarBlock* CHERRY_LOG;
    static RotatedPillarBlock* PALE_OAK_LOG;

    // =========================================================================
    // Blocks with properties - Wood (stripped logs, etc.)
    // =========================================================================
    static RotatedPillarBlock* STRIPPED_OAK_LOG;
    static RotatedPillarBlock* STRIPPED_SPRUCE_LOG;
    static RotatedPillarBlock* STRIPPED_BIRCH_LOG;
    static RotatedPillarBlock* STRIPPED_JUNGLE_LOG;
    static RotatedPillarBlock* STRIPPED_ACACIA_LOG;
    static RotatedPillarBlock* STRIPPED_DARK_OAK_LOG;
    static RotatedPillarBlock* STRIPPED_MANGROVE_LOG;
    static RotatedPillarBlock* STRIPPED_CHERRY_LOG;
    static RotatedPillarBlock* STRIPPED_PALE_OAK_LOG;

    /**
     * Bootstrap - Initialize all blocks
     * Reference: Blocks.java static initializer
     *
     * Must be called before using any blocks.
     */
    static void bootstrap();

    /**
     * Check if blocks have been initialized
     */
    static bool isInitialized();

    /**
     * Get block by name
     * @param name Block identifier like "minecraft:stone"
     * @return Block pointer or nullptr if not found
     */
    static Block* getBlock(const std::string& name);

    /**
     * Get default block state by name
     * @param name Block identifier like "minecraft:stone"
     * @return Default BlockState or nullptr if not found
     */
    static BlockState* getDefaultState(const std::string& name);

private:
    static bool s_initialized;
    static std::unordered_map<std::string, Block*> s_blocksByName;

    /**
     * Register a block
     */
    static void registerBlock(const std::string& name, Block* block);

    /**
     * Helper to create a simple block (no properties)
     */
    static Block* createSimpleBlock(const std::string& name);

    /**
     * Helper to create an air block
     */
    static Block* createAirBlock(const std::string& name);

    /**
     * Helper to create a liquid block (water, lava)
     */
    static Block* createLiquidBlock(const std::string& name);

    /**
     * Helper to create a non-solid vegetation block (grass, flowers)
     */
    static Block* createPlantBlock(const std::string& name);

    /**
     * Helper to create a non-solid vegetation block replaceable by trees
     * Reference: BlockTags.REPLACEABLE_BY_TREES
     */
    static Block* createReplaceableByTreesBlock(const std::string& name);

    /**
     * Helper to create a leaves block
     */
    static LeavesBlock* createLeavesBlock(const std::string& name);

    /**
     * Helper to create a log block (RotatedPillarBlock)
     */
    static RotatedPillarBlock* createLogBlock(const std::string& name);
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
