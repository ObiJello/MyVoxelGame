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

class HangingRootsBlock : public Block {
public:
    static inline BooleanProperty* WATERLOGGED = nullptr;

    explicit HangingRootsBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        const core::BlockPos attachedToPos = pos.above();
        BlockState* attachedToState = level.getBlockState(attachedToPos);
        return attachedToState &&
               attachedToState->isFaceSturdy(level, attachedToPos, core::Direction::DOWN);
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(WATERLOGGED);
    }

private:
    static void initializeProperties() {
        if (!WATERLOGGED) {
            BlockStateProperties::initialize();
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
