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

class SculkShriekerBlock : public Block {
public:
    static inline BooleanProperty* SHRIEKING = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;
    static inline BooleanProperty* CAN_SUMMON = nullptr;

    explicit SculkShriekerBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*SHRIEKING, false);
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            defaultState = defaultState->setValue(*CAN_SUMMON, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(SHRIEKING, WATERLOGGED, CAN_SUMMON);
    }

private:
    static void initializeProperties() {
        if (!SHRIEKING) {
            BlockStateProperties::initialize();
            SHRIEKING = BlockStateProperties::SHRIEKING;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
            CAN_SUMMON = BlockStateProperties::CAN_SUMMON;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
