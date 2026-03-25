#pragma once

#include "StateHolder.h"
#include "StateDefinition.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <string>
#include <unordered_map>

namespace minecraft {
namespace levelgen {
class WorldGenLevel;
}
}

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
 *
 * States are created by StateDefinition and are immutable.
 * Use setValue() to get a new state with a different value.
 */
class BlockState : public StateHolder<Block, BlockState> {
private:
    // Cached block properties
    // Reference: BlockBehaviour.BlockStateBase fields lines 748-770
    bool m_isAir;
    bool m_liquid;
    bool m_blocksMotion;
    bool m_forceSolidOff;
    bool m_forceSolidOn;
    bool m_noOcclusion;
    bool m_isReplaceable;
    bool m_isLeaves;
    bool m_isLog;
    bool m_isReplaceableByTrees;
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
    // Block type query methods
    // =========================================================================

    bool isAir() const { return m_isAir; }
    bool isFluid() const { return m_liquid; }
    bool blocksMotion() const;
    bool isLeaves() const { return m_isLeaves; }
    bool isLog() const { return m_isLog; }
    bool isReplaceableByTrees() const { return m_isReplaceableByTrees; }
    std::string getIdentifier() const { return m_identifier; }

    /**
     * Alias for getIdentifier() for compatibility
     */
    std::string getBlockName() const { return m_identifier; }

    /**
     * Check if this state is of the same block type as another state
     * Reference: BlockState.java is(BlockState)
     */
    bool is(const BlockState* other) const {
        if (this == other) return true;
        if (!other) return false;
        return m_identifier == other->m_identifier;
    }

    /**
     * Check if this state is of a specific block
     * Reference: BlockState.java is(Block)
     */
    bool is(const Block* block) const;

    /**
     * Get properties as string map for serialization
     */
    std::unordered_map<std::string, std::string> getProperties() const;

    bool hasProperties() const {
        return !m_values.empty();
    }

    bool equals(const BlockState* other) const;

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
     * Check if this state renders as a full solid opaque cube
     * Reference: BlockBehaviour.BlockStateBase.isSolidRender()
     * True for: stone, dirt, grass_block, sand, gravel, ores, deepslate, etc.
     * False for: air, leaves, logs, water, flowers, glass, partial blocks, etc.
     */
    bool isSolidRender() const;

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

    bool canBeReplaced() const { return m_isReplaceable; }
    bool hasWaterFluid() const;
    bool hasAnyFluid() const;

    bool canSurvive(const minecraft::levelgen::WorldGenLevel& level, const core::BlockPos& pos) const;

    bool isCollisionShapeFullBlock(const minecraft::levelgen::WorldGenLevel& level, const core::BlockPos& pos) const;

    bool isFaceSturdy(
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        core::Direction direction
    ) const;

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
