#pragma once

#include "world/level/block/blocks/BushBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::DoubleBlockHalf;
using state::properties::EnumProperty;

class DoublePlantBlock : public BushBlock {
public:
    static inline EnumProperty<DoubleBlockHalf>* HALF = nullptr;

    explicit DoublePlantBlock(const Properties& properties)
        : BushBlock(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*HALF, DoubleBlockHalf::lower());
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

        if (state->getValue(*HALF) != DoubleBlockHalf::upper()) {
            return BushBlock::canSurvive(state, level, pos);
        }

        BlockState* belowState = level.getBlockState(pos.below());
        return belowState && belowState->is(this) && belowState->getValue(*HALF) == DoubleBlockHalf::lower();
    }

    static void placeAt(
        levelgen::WorldGenLevel* level,
        BlockState* state,
        const core::BlockPos& lowerPos,
        int updateFlags
    ) {
        if (!level || !state) {
            return;
        }

        level->setBlock(lowerPos, copyWaterloggedFrom(*level, lowerPos, state->setValue(*HALF, DoubleBlockHalf::lower())), updateFlags);
        level->setBlock(lowerPos.above(), copyWaterloggedFrom(*level, lowerPos.above(), state->setValue(*HALF, DoubleBlockHalf::upper())), updateFlags);
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(HALF);
    }

private:
    static BlockState* copyWaterloggedFrom(
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        BlockState* state
    ) {
        if (!state || !BlockStateProperties::WATERLOGGED || !state->hasProperty(BlockStateProperties::WATERLOGGED)) {
            return state;
        }

        bool waterlogged = level.isWaterAt(pos);
        return state->setValue(*BlockStateProperties::WATERLOGGED, waterlogged);
    }

    static void initializeProperties() {
        if (!HALF) {
            BlockStateProperties::initialize();
            HALF = BlockStateProperties::DOUBLE_BLOCK_HALF;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
