#pragma once

#include "levelgen/WorldGenLevel.h"
#include "world/level/block/blocks/DoublePlantBlock.h"
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
using state::properties::EnumProperty;
using state::properties::DoubleBlockHalf;

class SmallDripleafBlock : public DoublePlantBlock {
public:
    static inline BooleanProperty* WATERLOGGED = nullptr;
    static inline DirectionProperty* FACING = nullptr;

    explicit SmallDripleafBlock(const Properties& properties)
        : DoublePlantBlock(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*HALF, DoubleBlockHalf::lower());
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* state,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        if (!state) {
            return false;
        }

        if (state->getValue(*HALF) == DoubleBlockHalf::upper()) {
            return DoublePlantBlock::canSurvive(state, level, pos);
        }

        core::BlockPos belowPos = pos.below();
        return mayPlaceOn(level.getBlockState(belowPos), level, belowPos);
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(HALF, WATERLOGGED, FACING);
    }

private:
    bool mayPlaceOn(
        BlockState* stateBelow,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& belowPos
    ) const {
        if (!stateBelow) {
            return false;
        }

        const std::string& belowName = stateBelow->getIdentifier();
        if (belowName == "minecraft:clay" || belowName == "minecraft:moss_block") {
            return true;
        }

        BlockState* aboveState = level.getBlockState(belowPos.above());
        return aboveState && aboveState->getIdentifier() == "minecraft:water" && BushBlock::mayPlaceOn(stateBelow);
    }

    static void initializeProperties() {
        if (!WATERLOGGED) {
            BlockStateProperties::initialize();
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
            FACING = BlockStateProperties::HORIZONTAL_FACING;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
