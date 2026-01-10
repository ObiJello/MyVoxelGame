#pragma once

#include "world/level/block/state/StateDefinition.h"
#include "world/level/block/state/BlockState.h"
#include "core/IdMapper.h"
#include <string>
#include <memory>

namespace minecraft {
namespace world {
namespace level {
namespace block {

// Forward declarations
namespace state {
class BlockState;
}

using state::BlockState;
using state::StateDefinition;

/**
 * Block - Base class for all block types
 * Reference: net/minecraft/world/level/block/Block.java
 *
 * Each Block instance has a StateDefinition that defines all possible
 * states, and a default BlockState. Subclasses override createBlockStateDefinition
 * to add properties.
 */
class Block {
public:
    /**
     * Block state registry - maps all block states to integer IDs
     * Reference: Block.java line 82
     */
    static core::IdMapper<BlockState*> BLOCK_STATE_REGISTRY;

    /**
     * Properties class for block configuration
     * Reference: BlockBehaviour.java Properties inner class
     */
    class Properties {
    private:
        bool m_isAir = false;
        bool m_liquid = false;
        bool m_blocksMotion = true;
        bool m_isLeaves = false;
        std::string m_identifier;

    public:
        Properties() = default;

        Properties& air() {
            m_isAir = true;
            m_blocksMotion = false;
            return *this;
        }

        Properties& liquid() {
            m_liquid = true;
            return *this;
        }

        Properties& noCollission() {
            m_blocksMotion = false;
            return *this;
        }

        Properties& leaves() {
            m_isLeaves = true;
            return *this;
        }

        Properties& setId(const std::string& id) {
            m_identifier = id;
            return *this;
        }

        bool isAir() const { return m_isAir; }
        bool isLiquid() const { return m_liquid; }
        bool blocksMotion() const { return m_blocksMotion; }
        bool isLeaves() const { return m_isLeaves; }
        const std::string& getIdentifier() const { return m_identifier; }
    };

protected:
    /**
     * State definition - contains all possible states
     * Reference: Block.java line 105
     */
    std::unique_ptr<StateDefinition<Block, BlockState>> m_stateDefinition;

    /**
     * Default block state
     * Reference: Block.java line 106
     */
    BlockState* m_defaultBlockState = nullptr;

    /**
     * Block properties
     */
    Properties m_properties;

    /**
     * Block identifier (e.g., "minecraft:stone")
     */
    std::string m_identifier;

public:
    /**
     * Constructor
     * Reference: Block.java Block(Properties) lines 224-238
     */
    explicit Block(const Properties& properties);

    virtual ~Block() = default;

    /**
     * Get block ID for a state
     * Reference: Block.java getId(BlockState) lines 122-129
     */
    static int getId(BlockState* blockState);

    /**
     * Get block state by ID
     * Reference: Block.java stateById(int) lines 131-134
     */
    static BlockState* stateById(int idWithData);

    /**
     * Create the block state definition
     * Reference: Block.java createBlockStateDefinition(Builder) line 465
     *
     * Override in subclasses to add properties.
     */
    virtual void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder);

    /**
     * Get the state definition
     * Reference: Block.java getStateDefinition() lines 468-470
     */
    const StateDefinition<Block, BlockState>& getStateDefinition() const;

    /**
     * Register the default state
     * Reference: Block.java registerDefaultState(BlockState) lines 472-474
     */
    void registerDefaultState(BlockState* state);

    /**
     * Get the default block state
     * Reference: Block.java defaultBlockState() lines 476-478
     */
    BlockState* defaultBlockState() const;

    /**
     * Get block identifier
     */
    const std::string& getIdentifier() const { return m_identifier; }

    /**
     * Get block properties
     */
    const Properties& getProperties() const { return m_properties; }

    /**
     * Check if this is an air block
     */
    bool isAir() const { return m_properties.isAir(); }

    /**
     * Check if this is a liquid block
     */
    bool isLiquid() const { return m_properties.isLiquid(); }

    /**
     * Check if this block blocks motion
     */
    bool blocksMotion() const { return m_properties.blocksMotion(); }

    /**
     * Check if this is a leaves block
     */
    bool isLeaves() const { return m_properties.isLeaves(); }

protected:
    /**
     * Build the state definition
     * Called by constructor after createBlockStateDefinition
     */
    void buildStateDefinition();
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

// Convenience alias
namespace minecraft {
    using Block = world::level::block::Block;
}
