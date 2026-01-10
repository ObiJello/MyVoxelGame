#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/blocks/StairBlock.h"
#include "world/level/block/blocks/SlabBlock.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/blocks/DoorBlock.h"
#include "world/level/block/blocks/WallBlock.h"
#include "world/level/block/blocks/LeavesBlock.h"
#include "world/level/block/blocks/RotatedPillarBlock.h"
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
    static Block* STONE;
    static Block* GRANITE;
    static Block* DIORITE;
    static Block* ANDESITE;
    static Block* DEEPSLATE;
    static Block* COBBLESTONE;
    static Block* DIRT;
    static Block* GRASS_BLOCK;
    static Block* SAND;
    static Block* GRAVEL;
    static Block* BEDROCK;
    static Block* WATER;
    static Block* LAVA;

    // =========================================================================
    // Blocks with properties
    // =========================================================================
    static StairBlock* OAK_STAIRS;
    static StairBlock* STONE_STAIRS;
    static StairBlock* COBBLESTONE_STAIRS;

    static SlabBlock* OAK_SLAB;
    static SlabBlock* STONE_SLAB;
    static SlabBlock* COBBLESTONE_SLAB;

    static FenceBlock* OAK_FENCE;
    static FenceBlock* NETHER_BRICK_FENCE;

    static DoorBlock* OAK_DOOR;
    static DoorBlock* IRON_DOOR;

    static WallBlock* COBBLESTONE_WALL;
    static WallBlock* STONE_BRICK_WALL;

    static LeavesBlock* OAK_LEAVES;
    static LeavesBlock* SPRUCE_LEAVES;
    static LeavesBlock* BIRCH_LEAVES;

    static RotatedPillarBlock* OAK_LOG;
    static RotatedPillarBlock* SPRUCE_LOG;
    static RotatedPillarBlock* BIRCH_LOG;

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
     * Create a simple block with no properties
     */
    template<typename... Args>
    static Block* createBlock(const std::string& name, Args&&... args) {
        Block::Properties props;
        props.setId(name);
        auto block = new Block(props);
        registerBlock(name, block);
        return block;
    }

    /**
     * Create a block with properties
     */
    template<typename T, typename... Args>
    static T* createBlock(const std::string& name, Args&&... args) {
        Block::Properties props;
        props.setId(name);
        auto block = new T(props);
        registerBlock(name, block);
        return block;
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
