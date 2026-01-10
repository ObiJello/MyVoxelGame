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
using state::properties::IntegerProperty;

/**
 * LeavesBlock - Tree leaves block
 * Reference: net/minecraft/world/level/block/LeavesBlock.java
 *
 * Has properties: DISTANCE, PERSISTENT, WATERLOGGED
 * Creates 7 * 2 * 2 = 28 states
 */
class LeavesBlock : public Block {
public:
    static inline IntegerProperty* DISTANCE = nullptr;
    static inline BooleanProperty* PERSISTENT = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    explicit LeavesBlock(const Properties& properties)
        : Block(Properties(properties).leaves()) {
        initializeProperties();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*DISTANCE, 7);
            defaultState = defaultState->setValue(*PERSISTENT, false);
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(DISTANCE, PERSISTENT, WATERLOGGED);
    }

private:
    static void initializeProperties() {
        if (!DISTANCE) {
            BlockStateProperties::initialize();
            DISTANCE = BlockStateProperties::DISTANCE;
            PERSISTENT = BlockStateProperties::PERSISTENT;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
