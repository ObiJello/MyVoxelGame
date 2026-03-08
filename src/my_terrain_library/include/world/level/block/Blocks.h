#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/blocks/StairBlock.h"
#include "world/level/block/blocks/SlabBlock.h"
#include "world/level/block/blocks/BushBlock.h"
#include "world/level/block/blocks/DoublePlantBlock.h"
#include "world/level/block/blocks/EyeblossomBlock.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/blocks/DoorBlock.h"
#include "world/level/block/blocks/WallBlock.h"
#include "world/level/block/blocks/LeavesBlock.h"
#include "world/level/block/blocks/LeafLitterBlock.h"
#include "world/level/block/blocks/FlowerBedBlock.h"
#include "world/level/block/blocks/CarpetBlock.h"
#include "world/level/block/blocks/RotatedPillarBlock.h"
#include "world/level/block/blocks/VineBlock.h"
#include "world/level/block/blocks/AzaleaBlock.h"
#include "world/level/block/blocks/CaveVinesBlock.h"
#include "world/level/block/blocks/CaveVinesPlantBlock.h"
#include "world/level/block/blocks/GlowLichenBlock.h"
#include "world/level/block/blocks/SporeBlossomBlock.h"
#include "world/level/block/blocks/HangingRootsBlock.h"
#include "world/level/block/blocks/SmallDripleafBlock.h"
#include "world/level/block/blocks/BigDripleafBlock.h"
#include "world/level/block/blocks/BigDripleafStemBlock.h"
#include "world/level/block/blocks/SculkBlock.h"
#include "world/level/block/blocks/SculkSensorBlock.h"
#include "world/level/block/blocks/SculkShriekerBlock.h"
#include "world/level/block/blocks/SculkVeinBlock.h"
#include "world/level/block/blocks/TallFlowerBlock.h"
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
    static Block* ROOTED_DIRT;
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
    static Block* MUDDY_MANGROVE_ROOTS;
    static Block* MAGMA_BLOCK;

    // =========================================================================
    // Sculk blocks (deep dark biome)
    // Reference: Used in sculk patch feature
    // =========================================================================
    static SculkBlock* SCULK;
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
    static BushBlock* SHORT_GRASS;
    static DoublePlantBlock* TALL_GRASS;
    static BushBlock* FERN;
    static DoublePlantBlock* LARGE_FERN;
    static BushBlock* DEAD_BUSH;
    static BushBlock* BUSH;

    // =========================================================================
    // Flowers (no collision)
    // =========================================================================
    static BushBlock* DANDELION;
    static BushBlock* POPPY;
    static BushBlock* BLUE_ORCHID;
    static BushBlock* ALLIUM;
    static BushBlock* AZURE_BLUET;
    static BushBlock* RED_TULIP;
    static BushBlock* ORANGE_TULIP;
    static BushBlock* WHITE_TULIP;
    static BushBlock* PINK_TULIP;
    static BushBlock* OXEYE_DAISY;
    static BushBlock* CORNFLOWER;
    static BushBlock* LILY_OF_THE_VALLEY;

    // =========================================================================
    // Tall flowers (two-block, no collision)
    // =========================================================================
    static TallFlowerBlock* SUNFLOWER;
    static TallFlowerBlock* LILAC;
    static TallFlowerBlock* ROSE_BUSH;
    static TallFlowerBlock* PEONY;

    // =========================================================================
    // Mushrooms (small, no collision)
    // =========================================================================
    static BushBlock* BROWN_MUSHROOM;
    static BushBlock* RED_MUSHROOM;

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
    static FlowerBedBlock* PINK_PETALS;
    static FlowerBedBlock* WILDFLOWERS;
    static Block* VINE;

    // =========================================================================
    // Moss and lush cave vegetation
    // Reference: Used in lush caves biome features
    // =========================================================================
    static Block* MOSS_BLOCK;
    static Block* MOSS_CARPET;
    static Block* CAVE_VINES;
    static Block* CAVE_VINES_PLANT;
    static GlowLichenBlock* GLOW_LICHEN;
    static Block* AZALEA;
    static Block* FLOWERING_AZALEA;
    static HangingRootsBlock* HANGING_ROOTS;
    static SporeBlossomBlock* SPORE_BLOSSOM;
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
    static EyeblossomBlock* CLOSED_EYEBLOSSOM;

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
    static Block* COCOA;
    static Block* MANGROVE_ROOTS;

    // =========================================================================
    // Tree saplings and propagules
    // Reference: Blocks.java lines 1339-1347
    // =========================================================================
    static Block* OAK_SAPLING;
    static Block* SPRUCE_SAPLING;
    static Block* BIRCH_SAPLING;
    static Block* JUNGLE_SAPLING;
    static Block* ACACIA_SAPLING;
    static Block* CHERRY_SAPLING;
    static Block* DARK_OAK_SAPLING;
    static Block* PALE_OAK_SAPLING;
    static Block* MANGROVE_PROPAGULE;

    // =========================================================================
    // Dungeon blocks
    // Reference: Used in MonsterRoomFeature
    // =========================================================================
    static Block* SPAWNER;
    static Block* CHEST;
    static Block* BEE_NEST;

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
    static Block* createReplaceablePlantBlock(const std::string& name);
    static CarpetBlock* createCarpetBlock(const std::string& name);
    static BushBlock* createBushBlock(const std::string& name);
    static VineBlock* createVineBlock(const std::string& name);
    static AzaleaBlock* createAzaleaBlock(const std::string& name);
    static CaveVinesBlock* createCaveVinesBlock(const std::string& name);
    static CaveVinesPlantBlock* createCaveVinesPlantBlock(const std::string& name);
    static GlowLichenBlock* createGlowLichenBlock(const std::string& name);
    static HangingRootsBlock* createHangingRootsBlock(const std::string& name);
    static SporeBlossomBlock* createSporeBlossomBlock(const std::string& name);
    static SmallDripleafBlock* createSmallDripleafBlock(const std::string& name);
    static BigDripleafBlock* createBigDripleafBlock(const std::string& name);
    static BigDripleafStemBlock* createBigDripleafStemBlock(const std::string& name);
    static DoublePlantBlock* createDoublePlantBlock(const std::string& name);
    static TallFlowerBlock* createTallFlowerBlock(const std::string& name);
    static FlowerBedBlock* createFlowerBedBlock(const std::string& name);
    static EyeblossomBlock* createEyeblossomBlock(const std::string& name, bool open);

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
