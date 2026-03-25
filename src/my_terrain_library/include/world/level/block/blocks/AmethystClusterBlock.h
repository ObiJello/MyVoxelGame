#pragma once

#include "levelgen/WorldGenLevel.h"
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
using state::properties::DirectionProperty;

class AmethystClusterBlock : public Block {
public:
    static inline BooleanProperty* WATERLOGGED = nullptr;
    static inline DirectionProperty* FACING = nullptr;

    explicit AmethystClusterBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            defaultState = defaultState->setValue(*FACING, core::Direction::UP);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* state,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        if (!state || !FACING || !state->hasProperty(FACING)) {
            return false;
        }

        core::Direction direction = state->getValueOrElse(*FACING, core::Direction::UP);
        core::BlockPos attachedToPos = pos.relative(core::getOpposite(direction));
        BlockState* attachedToState = level.getBlockState(attachedToPos);
        return attachedToState &&
               attachedToState->isFaceSturdy(level, attachedToPos, direction);
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(WATERLOGGED, FACING);
    }

private:
    static void initializeProperties() {
        if (!WATERLOGGED) {
            BlockStateProperties::initialize();
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
            FACING = BlockStateProperties::FACING;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
