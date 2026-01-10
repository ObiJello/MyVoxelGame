#pragma once

#include "StateHolder.h"
#include "StateDefinition.h"
#include "world/IBlockType.h"
#include <string>
#include <unordered_map>

namespace minecraft {
namespace world {
namespace level {
namespace block {

// Forward declarations
class Block;

namespace state {

/**
 * BlockState - Immutable block state with property values
 * Reference: net/minecraft/world/level/block/state/BlockState.java
 *
 * Combines BlockBehaviour.BlockStateBase functionality.
 * Implements IBlockType for compatibility with existing terrain generation.
 *
 * States are created by StateDefinition and are immutable.
 * Use setValue() to get a new state with a different value.
 */
class BlockState : public StateHolder<Block, BlockState>, public ::world::IBlockType {
private:
    // Cached block properties
    // Reference: BlockBehaviour.BlockStateBase fields lines 748-770
    bool m_isAir;
    bool m_liquid;
    bool m_blocksMotion;
    bool m_isLeaves;
    std::string m_identifier;  // Block name like "minecraft:stone"

public:
    /**
     * Constructor
     * Reference: BlockState.java BlockState(Block, Map, MapCodec)
     */
    BlockState(Block* owner, const ValueMap& values);

    /**
     * Get this as a BlockState
     * Reference: BlockState.java asState()
     */
    BlockState* asState() { return this; }

    /**
     * Get the block that owns this state
     * Reference: BlockBehaviour.BlockStateBase.getBlock()
     */
    Block* getBlock() const { return m_owner; }

    // =========================================================================
    // IBlockType interface implementation
    // =========================================================================

    bool isAir() const override { return m_isAir; }
    bool isFluid() const override { return m_liquid; }
    bool blocksMotion() const override { return m_blocksMotion; }
    bool isLeaves() const override { return m_isLeaves; }
    std::string getIdentifier() const override { return m_identifier; }

    /**
     * Get properties as string map for serialization
     */
    std::unordered_map<std::string, std::string> getProperties() const override;

    bool hasProperties() const override {
        return !m_values.empty();
    }

    bool equals(const ::world::IBlockType* other) const override;

    // =========================================================================
    // Block state specific methods
    // Reference: BlockBehaviour.BlockStateBase methods
    // =========================================================================

    /**
     * Initialize cached values
     * Reference: BlockBehaviour.BlockStateBase.initCache() lines 832-856
     */
    void initCache();

    /**
     * Check if this state is solid
     * Reference: BlockBehaviour.BlockStateBase.isSolid()
     */
    bool isSolid() const;

    /**
     * Check if this state can occlude light
     * Reference: BlockBehaviour.BlockStateBase.canOcclude()
     */
    bool canOcclude() const;

    /**
     * Get light emission level (0-15)
     * Reference: BlockBehaviour.BlockStateBase.getLightEmission()
     */
    int getLightEmission() const;

private:
    /**
     * Set cached values based on block type
     */
    void setCachedValues();
};

} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

// Convenience alias
namespace minecraft {
    using BlockState = world::level::block::state::BlockState;
}
