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

class BigDripleafStemBlock : public Block {
public:
    static inline BooleanProperty* WATERLOGGED = nullptr;
    static inline DirectionProperty* FACING = nullptr;

    explicit BigDripleafStemBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* /*state*/,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        BlockState* belowState = level.getBlockState(pos.below());
        BlockState* aboveState = level.getBlockState(pos.above());
        return belowState && aboveState &&
               (belowState->is(this) || isBigDripleafPlaceable(belowState)) &&
               (aboveState->is(this) || aboveState->getIdentifier() == "minecraft:big_dripleaf");
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(WATERLOGGED, FACING);
    }

private:
    static bool isBigDripleafPlaceable(BlockState* state) {
        if (!state) {
            return false;
        }

        const std::string& name = state->getIdentifier();
        return name == "minecraft:clay" ||
               name == "minecraft:moss_block" ||
               name == "minecraft:dirt" ||
               name == "minecraft:grass_block" ||
               name == "minecraft:podzol" ||
               name == "minecraft:coarse_dirt" ||
               name == "minecraft:mycelium" ||
               name == "minecraft:rooted_dirt" ||
               name == "minecraft:mud" ||
               name == "minecraft:muddy_mangrove_roots" ||
               name == "minecraft:farmland";
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
