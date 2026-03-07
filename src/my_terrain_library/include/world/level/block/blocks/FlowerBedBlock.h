#pragma once

#include "world/level/block/blocks/BushBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

// Reference: net/minecraft/world/level/block/FlowerBedBlock.java

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::IntegerProperty;
using state::properties::DirectionProperty;

class FlowerBedBlock : public BushBlock {
public:
    static inline DirectionProperty* FACING = nullptr;
    static inline IntegerProperty* FLOWER_AMOUNT = nullptr;

    explicit FlowerBedBlock(const Properties& properties)
        : BushBlock(Properties(properties)) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*FLOWER_AMOUNT, 1);
            registerDefaultState(defaultState);
        }
    }

    BlockState* getState(int amount, core::Direction facing) {
        BlockState* state = defaultBlockState();
        if (state && FACING && FLOWER_AMOUNT) {
            state = state->setValue(*FACING, facing);
            state = state->setValue(*FLOWER_AMOUNT, amount);
        }
        return state;
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(FACING, FLOWER_AMOUNT);
    }

private:
    static void initializeProperties() {
        if (!FACING) {
            BlockStateProperties::initialize();
            FACING = BlockStateProperties::HORIZONTAL_FACING;
            FLOWER_AMOUNT = BlockStateProperties::FLOWER_AMOUNT;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
