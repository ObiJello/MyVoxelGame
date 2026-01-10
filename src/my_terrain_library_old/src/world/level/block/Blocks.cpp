#include "world/level/block/Blocks.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

// Static member definitions
bool Blocks::s_initialized = false;
std::unordered_map<std::string, Block*> Blocks::s_blocksByName;

// Basic blocks
Block* Blocks::AIR = nullptr;
Block* Blocks::STONE = nullptr;
Block* Blocks::GRANITE = nullptr;
Block* Blocks::DIORITE = nullptr;
Block* Blocks::ANDESITE = nullptr;
Block* Blocks::DEEPSLATE = nullptr;
Block* Blocks::COBBLESTONE = nullptr;
Block* Blocks::DIRT = nullptr;
Block* Blocks::GRASS_BLOCK = nullptr;
Block* Blocks::SAND = nullptr;
Block* Blocks::GRAVEL = nullptr;
Block* Blocks::BEDROCK = nullptr;
Block* Blocks::WATER = nullptr;
Block* Blocks::LAVA = nullptr;

// Blocks with properties
StairBlock* Blocks::OAK_STAIRS = nullptr;
StairBlock* Blocks::STONE_STAIRS = nullptr;
StairBlock* Blocks::COBBLESTONE_STAIRS = nullptr;

SlabBlock* Blocks::OAK_SLAB = nullptr;
SlabBlock* Blocks::STONE_SLAB = nullptr;
SlabBlock* Blocks::COBBLESTONE_SLAB = nullptr;

FenceBlock* Blocks::OAK_FENCE = nullptr;
FenceBlock* Blocks::NETHER_BRICK_FENCE = nullptr;

DoorBlock* Blocks::OAK_DOOR = nullptr;
DoorBlock* Blocks::IRON_DOOR = nullptr;

WallBlock* Blocks::COBBLESTONE_WALL = nullptr;
WallBlock* Blocks::STONE_BRICK_WALL = nullptr;

LeavesBlock* Blocks::OAK_LEAVES = nullptr;
LeavesBlock* Blocks::SPRUCE_LEAVES = nullptr;
LeavesBlock* Blocks::BIRCH_LEAVES = nullptr;

RotatedPillarBlock* Blocks::OAK_LOG = nullptr;
RotatedPillarBlock* Blocks::SPRUCE_LOG = nullptr;
RotatedPillarBlock* Blocks::BIRCH_LOG = nullptr;

void Blocks::registerBlock(const std::string& name, Block* block) {
    s_blocksByName[name] = block;
}

void Blocks::bootstrap() {
    if (s_initialized) return;

    // Initialize BlockStateProperties first
    state::properties::BlockStateProperties::initialize();

    // Basic blocks
    // Reference: Blocks.java static initializer
    {
        Block::Properties props;
        props.setId("minecraft:air").air();
        AIR = new Block(props);
        registerBlock("minecraft:air", AIR);
    }

    {
        Block::Properties props;
        props.setId("minecraft:stone");
        STONE = new Block(props);
        registerBlock("minecraft:stone", STONE);
    }

    {
        Block::Properties props;
        props.setId("minecraft:granite");
        GRANITE = new Block(props);
        registerBlock("minecraft:granite", GRANITE);
    }

    {
        Block::Properties props;
        props.setId("minecraft:diorite");
        DIORITE = new Block(props);
        registerBlock("minecraft:diorite", DIORITE);
    }

    {
        Block::Properties props;
        props.setId("minecraft:andesite");
        ANDESITE = new Block(props);
        registerBlock("minecraft:andesite", ANDESITE);
    }

    {
        Block::Properties props;
        props.setId("minecraft:deepslate");
        DEEPSLATE = new Block(props);
        registerBlock("minecraft:deepslate", DEEPSLATE);
    }

    {
        Block::Properties props;
        props.setId("minecraft:cobblestone");
        COBBLESTONE = new Block(props);
        registerBlock("minecraft:cobblestone", COBBLESTONE);
    }

    {
        Block::Properties props;
        props.setId("minecraft:dirt");
        DIRT = new Block(props);
        registerBlock("minecraft:dirt", DIRT);
    }

    {
        Block::Properties props;
        props.setId("minecraft:grass_block");
        GRASS_BLOCK = new Block(props);
        registerBlock("minecraft:grass_block", GRASS_BLOCK);
    }

    {
        Block::Properties props;
        props.setId("minecraft:sand");
        SAND = new Block(props);
        registerBlock("minecraft:sand", SAND);
    }

    {
        Block::Properties props;
        props.setId("minecraft:gravel");
        GRAVEL = new Block(props);
        registerBlock("minecraft:gravel", GRAVEL);
    }

    {
        Block::Properties props;
        props.setId("minecraft:bedrock");
        BEDROCK = new Block(props);
        registerBlock("minecraft:bedrock", BEDROCK);
    }

    {
        Block::Properties props;
        props.setId("minecraft:water").liquid().noCollission();
        WATER = new Block(props);
        registerBlock("minecraft:water", WATER);
    }

    {
        Block::Properties props;
        props.setId("minecraft:lava").liquid().noCollission();
        LAVA = new Block(props);
        registerBlock("minecraft:lava", LAVA);
    }

    // Stairs
    {
        Block::Properties props;
        props.setId("minecraft:oak_stairs");
        OAK_STAIRS = new StairBlock(props);
        registerBlock("minecraft:oak_stairs", OAK_STAIRS);
    }

    {
        Block::Properties props;
        props.setId("minecraft:stone_stairs");
        STONE_STAIRS = new StairBlock(props);
        registerBlock("minecraft:stone_stairs", STONE_STAIRS);
    }

    {
        Block::Properties props;
        props.setId("minecraft:cobblestone_stairs");
        COBBLESTONE_STAIRS = new StairBlock(props);
        registerBlock("minecraft:cobblestone_stairs", COBBLESTONE_STAIRS);
    }

    // Slabs
    {
        Block::Properties props;
        props.setId("minecraft:oak_slab");
        OAK_SLAB = new SlabBlock(props);
        registerBlock("minecraft:oak_slab", OAK_SLAB);
    }

    {
        Block::Properties props;
        props.setId("minecraft:stone_slab");
        STONE_SLAB = new SlabBlock(props);
        registerBlock("minecraft:stone_slab", STONE_SLAB);
    }

    {
        Block::Properties props;
        props.setId("minecraft:cobblestone_slab");
        COBBLESTONE_SLAB = new SlabBlock(props);
        registerBlock("minecraft:cobblestone_slab", COBBLESTONE_SLAB);
    }

    // Fences
    {
        Block::Properties props;
        props.setId("minecraft:oak_fence");
        OAK_FENCE = new FenceBlock(props);
        registerBlock("minecraft:oak_fence", OAK_FENCE);
    }

    {
        Block::Properties props;
        props.setId("minecraft:nether_brick_fence");
        NETHER_BRICK_FENCE = new FenceBlock(props);
        registerBlock("minecraft:nether_brick_fence", NETHER_BRICK_FENCE);
    }

    // Doors
    {
        Block::Properties props;
        props.setId("minecraft:oak_door");
        OAK_DOOR = new DoorBlock(props);
        registerBlock("minecraft:oak_door", OAK_DOOR);
    }

    {
        Block::Properties props;
        props.setId("minecraft:iron_door");
        IRON_DOOR = new DoorBlock(props);
        registerBlock("minecraft:iron_door", IRON_DOOR);
    }

    // Walls
    {
        Block::Properties props;
        props.setId("minecraft:cobblestone_wall");
        COBBLESTONE_WALL = new WallBlock(props);
        registerBlock("minecraft:cobblestone_wall", COBBLESTONE_WALL);
    }

    {
        Block::Properties props;
        props.setId("minecraft:stone_brick_wall");
        STONE_BRICK_WALL = new WallBlock(props);
        registerBlock("minecraft:stone_brick_wall", STONE_BRICK_WALL);
    }

    // Leaves
    {
        Block::Properties props;
        props.setId("minecraft:oak_leaves");
        OAK_LEAVES = new LeavesBlock(props);
        registerBlock("minecraft:oak_leaves", OAK_LEAVES);
    }

    {
        Block::Properties props;
        props.setId("minecraft:spruce_leaves");
        SPRUCE_LEAVES = new LeavesBlock(props);
        registerBlock("minecraft:spruce_leaves", SPRUCE_LEAVES);
    }

    {
        Block::Properties props;
        props.setId("minecraft:birch_leaves");
        BIRCH_LEAVES = new LeavesBlock(props);
        registerBlock("minecraft:birch_leaves", BIRCH_LEAVES);
    }

    // Logs
    {
        Block::Properties props;
        props.setId("minecraft:oak_log");
        OAK_LOG = new RotatedPillarBlock(props);
        registerBlock("minecraft:oak_log", OAK_LOG);
    }

    {
        Block::Properties props;
        props.setId("minecraft:spruce_log");
        SPRUCE_LOG = new RotatedPillarBlock(props);
        registerBlock("minecraft:spruce_log", SPRUCE_LOG);
    }

    {
        Block::Properties props;
        props.setId("minecraft:birch_log");
        BIRCH_LOG = new RotatedPillarBlock(props);
        registerBlock("minecraft:birch_log", BIRCH_LOG);
    }

    s_initialized = true;
}

bool Blocks::isInitialized() {
    return s_initialized;
}

Block* Blocks::getBlock(const std::string& name) {
    auto it = s_blocksByName.find(name);
    return it != s_blocksByName.end() ? it->second : nullptr;
}

BlockState* Blocks::getDefaultState(const std::string& name) {
    Block* block = getBlock(name);
    return block ? block->defaultBlockState() : nullptr;
}

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
