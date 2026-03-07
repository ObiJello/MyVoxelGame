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
using state::properties::EnumProperty;
using state::properties::IntegerProperty;
using state::properties::BooleanProperty;
using state::properties::SculkSensorPhase;

class SculkSensorBlock : public Block {
public:
    static inline EnumProperty<SculkSensorPhase>* PHASE = nullptr;
    static inline IntegerProperty* POWER = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    explicit SculkSensorBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*PHASE, SculkSensorPhase::inactive());
            defaultState = defaultState->setValue(*POWER, 0);
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(PHASE, POWER, WATERLOGGED);
    }

private:
    static void initializeProperties() {
        if (!PHASE) {
            BlockStateProperties::initialize();
            PHASE = BlockStateProperties::SCULK_SENSOR_PHASE;
            POWER = BlockStateProperties::POWER;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
