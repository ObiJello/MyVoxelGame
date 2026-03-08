#pragma once

#include "core/Direction.h"
#include "levelgen/WorldGenLevel.h"
#include "world/level/block/Block.h"
#include "world/level/block/blocks/MultifaceBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::BooleanProperty;

class VineBlock : public Block {
public:
    static inline BooleanProperty* UP = nullptr;
    static inline BooleanProperty* NORTH = nullptr;
    static inline BooleanProperty* EAST = nullptr;
    static inline BooleanProperty* SOUTH = nullptr;
    static inline BooleanProperty* WEST = nullptr;

    explicit VineBlock(const Properties& properties)
        : Block(Properties(properties).noCollission().replaceable()) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*UP, false);
            defaultState = defaultState->setValue(*NORTH, false);
            defaultState = defaultState->setValue(*EAST, false);
            defaultState = defaultState->setValue(*SOUTH, false);
            defaultState = defaultState->setValue(*WEST, false);
            registerDefaultState(defaultState);
        }
    }

    static BooleanProperty* getPropertyForFace(core::Direction direction) {
        switch (direction) {
            case core::Direction::UP: return UP;
            case core::Direction::NORTH: return NORTH;
            case core::Direction::EAST: return EAST;
            case core::Direction::SOUTH: return SOUTH;
            case core::Direction::WEST: return WEST;
            default: return nullptr;
        }
    }

    static bool isAcceptableNeighbour(
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& neighbourPos,
        core::Direction directionToNeighbour
    ) {
        return MultifaceBlock::canAttachTo(level, directionToNeighbour, neighbourPos, level.getBlockState(neighbourPos));
    }

    bool canSurvive(
        BlockState* state,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        return hasFaces(getUpdatedState(state ? state : defaultBlockState(), level, pos));
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(UP, NORTH, EAST, SOUTH, WEST);
    }

private:
    bool hasFaces(BlockState* state) const {
        return state &&
               (state->getValue(*UP) || state->getValue(*NORTH) || state->getValue(*EAST) ||
                state->getValue(*SOUTH) || state->getValue(*WEST));
    }

    bool canSupportAtFace(
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        core::Direction direction
    ) const {
        if (direction == core::Direction::DOWN) {
            return false;
        }

        core::BlockPos relative = pos.relative(direction);
        if (isAcceptableNeighbour(level, relative, direction)) {
            return true;
        }

        if (core::getAxis(direction) == core::Axis::Y) {
            return false;
        }

        BooleanProperty* property = getPropertyForFace(direction);
        BlockState* aboveState = level.getBlockState(pos.above());
        return property && aboveState && aboveState->is(this) && aboveState->getValue(*property);
    }

    BlockState* getUpdatedState(
        BlockState* state,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const {
        if (!state) {
            return nullptr;
        }

        core::BlockPos abovePos = pos.above();
        if (state->getValue(*UP)) {
            state = state->setValue(*UP, isAcceptableNeighbour(level, abovePos, core::Direction::DOWN));
        }

        BlockState* aboveState = nullptr;
        for (core::Direction direction : {core::Direction::NORTH, core::Direction::EAST, core::Direction::SOUTH, core::Direction::WEST}) {
            BooleanProperty* property = getPropertyForFace(direction);
            if (!property || !state->getValue(*property)) {
                continue;
            }

            bool canSupport = canSupportAtFace(level, pos, direction);
            if (!canSupport) {
                if (!aboveState) {
                    aboveState = level.getBlockState(abovePos);
                }
                canSupport = aboveState && aboveState->is(this) && aboveState->getValue(*property);
            }

            state = state->setValue(*property, canSupport);
        }

        return state;
    }

    static void initializeProperties() {
        if (!UP) {
            BlockStateProperties::initialize();
            UP = BlockStateProperties::UP;
            NORTH = BlockStateProperties::NORTH;
            EAST = BlockStateProperties::EAST;
            SOUTH = BlockStateProperties::SOUTH;
            WEST = BlockStateProperties::WEST;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
