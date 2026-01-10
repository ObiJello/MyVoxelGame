#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "core/Direction.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::BooleanProperty;
using state::properties::DirectionProperty;
using state::properties::EnumProperty;
using state::properties::DoubleBlockHalf;
using state::properties::DoorHingeSide;

/**
 * DoorBlock - Two-block-tall door
 * Reference: net/minecraft/world/level/block/DoorBlock.java
 *
 * Has properties: FACING, OPEN, HINGE, POWERED, HALF
 * Creates 4 * 2 * 2 * 2 * 2 = 64 states
 */
class DoorBlock : public Block {
public:
    static inline DirectionProperty* FACING = nullptr;
    static inline BooleanProperty* OPEN = nullptr;
    static inline EnumProperty<DoorHingeSide>* HINGE = nullptr;
    static inline BooleanProperty* POWERED = nullptr;
    static inline EnumProperty<DoubleBlockHalf>* HALF = nullptr;

    explicit DoorBlock(const Properties& properties) : Block(properties) {
        initializeProperties();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*OPEN, false);
            defaultState = defaultState->setValue(*HINGE, DoorHingeSide::left());
            defaultState = defaultState->setValue(*POWERED, false);
            defaultState = defaultState->setValue(*HALF, DoubleBlockHalf::lower());
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(HALF, FACING, OPEN, HINGE, POWERED);
    }

private:
    static void initializeProperties() {
        if (!FACING) {
            BlockStateProperties::initialize();
            FACING = BlockStateProperties::HORIZONTAL_FACING;
            OPEN = BlockStateProperties::OPEN;
            HINGE = BlockStateProperties::DOOR_HINGE;
            POWERED = BlockStateProperties::POWERED;
            HALF = BlockStateProperties::DOUBLE_BLOCK_HALF;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
