#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::BooleanProperty;

/**
 * FenceBlock - Block with fence connections
 * Reference: net/minecraft/world/level/block/FenceBlock.java
 *
 * Has properties: NORTH, EAST, SOUTH, WEST, WATERLOGGED
 * Creates 2^5 = 32 states
 */
class FenceBlock : public Block {
public:
    // Static property references
    static inline BooleanProperty* NORTH = nullptr;
    static inline BooleanProperty* EAST = nullptr;
    static inline BooleanProperty* SOUTH = nullptr;
    static inline BooleanProperty* WEST = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    /**
     * Constructor
     */
    explicit FenceBlock(const Properties& properties) : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        // Set default state
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*NORTH, false);
            defaultState = defaultState->setValue(*EAST, false);
            defaultState = defaultState->setValue(*SOUTH, false);
            defaultState = defaultState->setValue(*WEST, false);
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    /**
     * Create block state definition
     */
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(NORTH, EAST, SOUTH, WEST, WATERLOGGED);
    }

private:
    static void initializeProperties() {
        if (!NORTH) {
            BlockStateProperties::initialize();
            NORTH = BlockStateProperties::NORTH;
            EAST = BlockStateProperties::EAST;
            SOUTH = BlockStateProperties::SOUTH;
            WEST = BlockStateProperties::WEST;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
