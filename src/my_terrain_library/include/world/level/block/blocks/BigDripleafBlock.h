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
using state::properties::EnumProperty;
using state::properties::Tilt;

class BigDripleafBlock : public Block {
public:
    static inline BooleanProperty* WATERLOGGED = nullptr;
    static inline DirectionProperty* FACING = nullptr;
    static inline EnumProperty<Tilt>* TILT = nullptr;

    explicit BigDripleafBlock(const Properties& properties)
        : Block(Properties(properties).noCollission().replaceable()) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*TILT, Tilt::none());
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* /*state*/,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        BlockState* belowState = level.getBlockState(pos.below());
        return belowState &&
               (belowState->is(this) ||
                belowState->getIdentifier() == "minecraft:big_dripleaf_stem" ||
                isBigDripleafPlaceable(belowState));
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(WATERLOGGED, FACING, TILT);
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
            TILT = BlockStateProperties::TILT;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
