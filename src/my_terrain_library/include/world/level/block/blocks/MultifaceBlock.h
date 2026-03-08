#pragma once

#include "core/Direction.h"
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

class MultifaceBlock : public Block {
public:
    static inline BooleanProperty* FACE_DOWN = nullptr;
    static inline BooleanProperty* FACE_UP = nullptr;
    static inline BooleanProperty* FACE_NORTH = nullptr;
    static inline BooleanProperty* FACE_SOUTH = nullptr;
    static inline BooleanProperty* FACE_WEST = nullptr;
    static inline BooleanProperty* FACE_EAST = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    explicit MultifaceBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*FACE_DOWN, false);
            defaultState = defaultState->setValue(*FACE_UP, false);
            defaultState = defaultState->setValue(*FACE_NORTH, false);
            defaultState = defaultState->setValue(*FACE_SOUTH, false);
            defaultState = defaultState->setValue(*FACE_WEST, false);
            defaultState = defaultState->setValue(*FACE_EAST, false);
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

    static BooleanProperty* getFaceProperty(core::Direction direction) {
        initializeProperties();
        switch (direction) {
            case core::Direction::DOWN: return FACE_DOWN;
            case core::Direction::UP: return FACE_UP;
            case core::Direction::NORTH: return FACE_NORTH;
            case core::Direction::SOUTH: return FACE_SOUTH;
            case core::Direction::WEST: return FACE_WEST;
            case core::Direction::EAST: return FACE_EAST;
            default: return nullptr;
        }
    }

    static bool hasFace(const BlockState* state, core::Direction faceDirection) {
        if (!state) {
            return false;
        }

        if (const BooleanProperty* property = getFaceProperty(faceDirection)) {
            return state->getValueOrElse(*property, false);
        }
        return false;
    }

    static bool hasAnyFace(const BlockState* state) {
        static constexpr core::Direction kAllDirections[] = {
            core::Direction::DOWN,
            core::Direction::UP,
            core::Direction::NORTH,
            core::Direction::SOUTH,
            core::Direction::WEST,
            core::Direction::EAST
        };

        for (core::Direction direction : kAllDirections) {
            if (hasFace(state, direction)) {
                return true;
            }
        }

        return false;
    }

    static bool canAttachTo(
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        core::Direction directionTowardsNeighbour
    ) {
        const core::BlockPos neighbourPos = pos.relative(directionTowardsNeighbour);
        BlockState* neighbourState = level.getBlockState(neighbourPos);
        return canAttachTo(level, directionTowardsNeighbour, neighbourPos, neighbourState);
    }

    static bool canAttachTo(
        const minecraft::levelgen::WorldGenLevel& level,
        core::Direction directionTowardsNeighbour,
        const core::BlockPos& neighbourPos,
        BlockState* neighbourState
    ) {
        return neighbourState &&
               neighbourState->isFaceSturdy(level, neighbourPos, core::getOpposite(directionTowardsNeighbour));
    }

    bool isValidStateForPlacement(
        const minecraft::levelgen::WorldGenLevel& level,
        BlockState* oldState,
        const core::BlockPos& placementPos,
        core::Direction placementDirection
    ) const {
        if (!isFaceSupported(placementDirection)) {
            return false;
        }

        if (oldState && oldState->is(this) && hasFace(oldState, placementDirection)) {
            return false;
        }

        const core::BlockPos neighbourPos = placementPos.relative(placementDirection);
        return canAttachTo(level, placementDirection, neighbourPos, level.getBlockState(neighbourPos));
    }

    BlockState* getStateForPlacement(
        BlockState* oldState,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& placementPos,
        core::Direction placementDirection
    ) const {
        if (!isValidStateForPlacement(level, oldState, placementPos, placementDirection)) {
            return nullptr;
        }

        BlockState* newState = nullptr;
        if (oldState && oldState->is(this)) {
            newState = oldState;
        } else if (oldState &&
                   (oldState->getIdentifier() == "minecraft:water" ||
                    oldState->getValueOrElse(*WATERLOGGED, false))) {
            newState = defaultBlockState()->setValue(*WATERLOGGED, true);
        } else {
            newState = defaultBlockState();
        }

        if (BooleanProperty* property = getFaceProperty(placementDirection)) {
            return newState->setValue(*property, true);
        }

        return nullptr;
    }

    bool canSurvive(
        BlockState* state,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        if (!state) {
            return false;
        }

        bool hasAtLeastOneFace = false;
        static constexpr core::Direction kAllDirections[] = {
            core::Direction::DOWN,
            core::Direction::UP,
            core::Direction::NORTH,
            core::Direction::SOUTH,
            core::Direction::WEST,
            core::Direction::EAST
        };

        for (core::Direction direction : kAllDirections) {
            if (hasFace(state, direction)) {
                if (!canAttachTo(level, pos, direction)) {
                    return false;
                }
                hasAtLeastOneFace = true;
            }
        }

        return hasAtLeastOneFace;
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        for (core::Direction direction : {
                 core::Direction::DOWN,
                 core::Direction::UP,
                 core::Direction::NORTH,
                 core::Direction::SOUTH,
                 core::Direction::WEST,
                 core::Direction::EAST
             }) {
            if (isFaceSupported(direction)) {
                builder.add(getFaceProperty(direction));
            }
        }
        builder.add(WATERLOGGED);
    }

    virtual bool isFaceSupported(core::Direction /*direction*/) const {
        return true;
    }

private:
    static void initializeProperties() {
        if (!FACE_DOWN) {
            BlockStateProperties::initialize();
            FACE_DOWN = BlockStateProperties::DOWN;
            FACE_UP = BlockStateProperties::UP;
            FACE_NORTH = BlockStateProperties::NORTH;
            FACE_SOUTH = BlockStateProperties::SOUTH;
            FACE_WEST = BlockStateProperties::WEST;
            FACE_EAST = BlockStateProperties::EAST;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
