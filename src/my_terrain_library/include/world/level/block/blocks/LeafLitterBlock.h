#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

// Reference: net/minecraft/world/level/block/LeafLitterBlock.java

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::IntegerProperty;
using state::properties::DirectionProperty;

/**
 * LeafLitterBlock - Leaf litter decoration block
 * Reference: net/minecraft/world/level/block/LeafLitterBlock.java
 *
 * Has properties: HORIZONTAL_FACING, SEGMENT_AMOUNT
 * Creates 4 (directions) * 4 (amounts) = 16 states
 */
class LeafLitterBlock : public Block {
public:
    static inline DirectionProperty* FACING = nullptr;
    static inline IntegerProperty* SEGMENT_AMOUNT = nullptr;

    explicit LeafLitterBlock(const Properties& properties)
        : Block(Properties(properties).noCollission()) {
        initializeProperties();
        rebuildStateDefinition();

        // Default: facing NORTH, amount 1
        // Reference: LeafLitterBlock.java line 25
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*SEGMENT_AMOUNT, 1);
            registerDefaultState(defaultState);
        }
    }

    /**
     * Get a state with specific facing and amount
     * Reference: Used by feature placement
     */
    BlockState* getState(int amount, core::Direction facing) {
        BlockState* state = defaultBlockState();
        if (state && FACING && SEGMENT_AMOUNT) {
            state = state->setValue(*FACING, facing);
            state = state->setValue(*SEGMENT_AMOUNT, amount);
        }
        return state;
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(FACING, SEGMENT_AMOUNT);
    }

private:
    static void initializeProperties() {
        if (!FACING) {
            BlockStateProperties::initialize();
            FACING = BlockStateProperties::HORIZONTAL_FACING;
            SEGMENT_AMOUNT = BlockStateProperties::SEGMENT_AMOUNT;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
