#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/SculkSpreader.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "core/Direction.h"
#include <set>

namespace minecraft {
namespace levelgen {
class WorldGenLevel;
}
}

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::BooleanProperty;
using core::Direction;

/**
 * SculkVeinBlock - Sculk vein multiface block
 * Reference: net/minecraft/world/level/block/SculkVeinBlock.java
 *
 * Has 6 face properties (DOWN, UP, NORTH, SOUTH, WEST, EAST) + WATERLOGGED
 * Reference: MultifaceBlock.java for face property handling
 */
class SculkVeinBlock : public Block, public SculkBehaviour {
public:
    // Face properties - one for each direction
    static inline BooleanProperty* FACE_DOWN = nullptr;
    static inline BooleanProperty* FACE_UP = nullptr;
    static inline BooleanProperty* FACE_NORTH = nullptr;
    static inline BooleanProperty* FACE_SOUTH = nullptr;
    static inline BooleanProperty* FACE_WEST = nullptr;
    static inline BooleanProperty* FACE_EAST = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    explicit SculkVeinBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        // Default state: all faces false, not waterlogged
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

    /**
     * Check if a block state has a face in the given direction
     * Reference: MultifaceBlock.hasFace() lines 228-231
     */
    static bool hasFace(const BlockState* state, Direction direction) {
        if (!state) return false;

        initializeProperties();
        BooleanProperty* prop = getFaceProperty(direction);
        if (!prop) return false;

        try {
            return state->getValueOrElse(*prop, false);
        } catch (...) {
            return false;
        }
    }

    /**
     * Get the property for a specific face direction
     * Reference: MultifaceBlock.getFaceProperty() lines 248-250
     */
    static BooleanProperty* getFaceProperty(Direction direction) {
        initializeProperties();
        switch (direction) {
            case Direction::DOWN:  return FACE_DOWN;
            case Direction::UP:    return FACE_UP;
            case Direction::NORTH: return FACE_NORTH;
            case Direction::SOUTH: return FACE_SOUTH;
            case Direction::WEST:  return FACE_WEST;
            case Direction::EAST:  return FACE_EAST;
            default: return nullptr;
        }
    }

    /**
     * Get state with an additional face
     * Reference: MultifaceBlock.getStateForPlacement() lines 186-194
     *
     * If current state already has the face, returns same state.
     * Otherwise returns state with the new face added.
     */
    static BlockState* getStateWithFace(BlockState* currentState, Direction face) {
        if (!currentState) return nullptr;

        BooleanProperty* prop = getFaceProperty(face);
        if (!prop) return currentState;

        try {
            bool alreadyHasFace = currentState->getValueOrElse(*prop, false);
            if (alreadyHasFace) {
                return currentState;
            }
            return currentState->setValue(*prop, true);
        } catch (...) {
            return currentState;
        }
    }

    /**
     * Get a set of all faces present on a state
     * Reference: MultifaceBlock.availableFaces() lines 71-84
     */
    static std::set<Direction> availableFaces(const BlockState* state) {
        std::set<Direction> faces;
        if (!state) return faces;

        static const Direction allDirs[] = {
            Direction::DOWN, Direction::UP, Direction::NORTH,
            Direction::SOUTH, Direction::WEST, Direction::EAST
        };

        for (Direction dir : allDirs) {
            if (hasFace(state, dir)) {
                faces.insert(dir);
            }
        }
        return faces;
    }

    static bool hasAnyFace(const BlockState* state) {
        return !availableFaces(state).empty();
    }

    static bool canAttachTo(
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        Direction attachDirection
    ) {
        core::BlockPos attachPos = pos.relative(attachDirection);
        BlockState* attachState = level.getBlockState(attachPos);
        return attachState && attachState->isFaceSturdy(level, attachPos, core::getOpposite(attachDirection));
    }

    bool isValidStateForPlacement(
        BlockState* oldState,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& placementPos,
        Direction placementDirection
    ) const {
        if (oldState && oldState->getIdentifier() == "minecraft:sculk_vein" && hasFace(oldState, placementDirection)) {
            return false;
        }

        return canAttachTo(level, placementPos, placementDirection);
    }

    BlockState* getStateForPlacement(
        BlockState* oldState,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& placementPos,
        Direction placementDirection
    ) const {
        if (!isValidStateForPlacement(oldState, level, placementPos, placementDirection)) {
            return nullptr;
        }

        BlockState* placementState = oldState && oldState->getIdentifier() == "minecraft:sculk_vein"
            ? oldState
            : defaultBlockState();
        if (!placementState) {
            return nullptr;
        }

        if (placementState->hasProperty(WATERLOGGED)) {
            bool waterlogged = oldState && oldState->hasAnyFluid();
            placementState = placementState->setValue(*WATERLOGGED, waterlogged);
        }

        return getStateWithFace(placementState, placementDirection);
    }

    bool regrow(
        minecraft::levelgen::WorldGenLevel* level,
        const core::BlockPos& pos,
        BlockState* existing,
        const std::set<Direction>& faces
    ) const {
        bool hasAtLeastOneFace = false;
        BlockState* newState = defaultBlockState();
        if (!newState) {
            return false;
        }

        for (Direction face : faces) {
            if (canAttachTo(*level, pos, face)) {
                newState = getStateWithFace(newState, face);
                hasAtLeastOneFace = true;
            }
        }

        if (!hasAtLeastOneFace) {
            return false;
        }

        if (existing && existing->hasAnyFluid()) {
            newState = newState->setValue(*WATERLOGGED, true);
        }

        return level->setBlock(pos, newState, 3);
    }

    int attemptUseCharge(ChargeCursor& cursor, minecraft::levelgen::WorldGenLevel* level,
                         const core::BlockPos& originPos, minecraft::levelgen::WorldgenRandom& random,
                         SculkSpreader& spreader, bool spreadVeins) const override;

    void onDischarged(minecraft::levelgen::WorldGenLevel* level, BlockState* state, const core::BlockPos& pos,
                      minecraft::levelgen::WorldgenRandom& random) const override;

    bool spreadAll(BlockState* state, minecraft::levelgen::WorldGenLevel* level,
                   const core::BlockPos& pos, bool postProcess) const;

    bool spreadFromFaceTowardRandomDirection(BlockState* state, minecraft::levelgen::WorldGenLevel* level,
                                             const core::BlockPos& pos, Direction startingFace,
                                             minecraft::levelgen::WorldgenRandom& random, bool postProcess) const;

    bool spreadSameSpace(BlockState* state, minecraft::levelgen::WorldGenLevel* level,
                         const core::BlockPos& pos, bool postProcess) const;

    static bool hasSubstrateAccess(minecraft::levelgen::WorldGenLevel* level, BlockState* state,
                                   const core::BlockPos& pos);

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(FACE_DOWN, FACE_UP, FACE_NORTH, FACE_SOUTH, FACE_WEST, FACE_EAST, WATERLOGGED);
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
