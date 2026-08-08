#pragma once

#include "levelgen/WorldGenLevel.h"
#include "random/XoroshiroRandomSource.h"
#include "world/level/block/Block.h"
#include "world/level/block/blocks/MultifaceBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::BooleanProperty;
using state::properties::EnumProperty;
using state::properties::WallSide;

class MossyCarpetBlock : public Block {
public:
    static inline std::unique_ptr<BooleanProperty> BASE_STORAGE = nullptr;
    static inline BooleanProperty* BASE = nullptr;
    static inline EnumProperty<WallSide>* NORTH = nullptr;
    static inline EnumProperty<WallSide>* EAST = nullptr;
    static inline EnumProperty<WallSide>* SOUTH = nullptr;
    static inline EnumProperty<WallSide>* WEST = nullptr;

    explicit MossyCarpetBlock(const Properties& properties)
        : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BASE, true);
            defaultState = defaultState->setValue(*NORTH, WallSide::none());
            defaultState = defaultState->setValue(*EAST, WallSide::none());
            defaultState = defaultState->setValue(*SOUTH, WallSide::none());
            defaultState = defaultState->setValue(*WEST, WallSide::none());
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* state,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        BlockState* belowState = level.getBlockState(pos.below());
        if (!belowState) {
            return false;
        }

        if (state && state->getValueOrElse(*BASE, false)) {
            return !belowState->isAir();
        }

        return belowState->is(this) && belowState->getValueOrElse(*BASE, false);
    }

    // Parity-debug: MC_CARPET_TRACE=<file> logs every placeAt's region-random
    // consumption (one nextBoolean per non-NONE candidate topper side) with
    // world context, independent of the per-feature RNG trace gating.
    static std::FILE* carpetTraceFile() {
        static std::FILE* f = [] {
            const char* p = std::getenv("MC_CARPET_TRACE");
            return (p && *p) ? std::fopen(p, "w") : nullptr;
        }();
        return f;
    }

    void placeAt(
        levelgen::WorldGenLevel* level,
        const core::BlockPos& pos,
        minecraft::XoroshiroRandomSource& random,
        int updateFlags
    ) const {
        BlockState* adjustedCarpetLayer = getUpdatedState(defaultBlockState(), *level, pos, true);
        if (!adjustedCarpetLayer) {
            return;
        }
        if (std::FILE* ct = carpetTraceFile()) {
            std::fprintf(ct, "CT PLACE %d,%d,%d rng=%p base=%s\n",
                         pos.getX(), pos.getY(), pos.getZ(),
                         static_cast<void*>(&random),
                         adjustedCarpetLayer->toStateString().c_str());
        }

        level->setBlock(pos, adjustedCarpetLayer, updateFlags);

        BlockState* topperState = createTopperWithSideChance(*level, pos, random);
        if (topperState) {
            level->setBlock(pos.above(), topperState, updateFlags);
            BlockState* updatedBottom = getUpdatedState(adjustedCarpetLayer, *level, pos, true);
            if (updatedBottom) {
                level->setBlock(pos, updatedBottom, updateFlags);
            }
        }
    }

    // Reference: MossyCarpetBlock.updateShape -> getUpdatedState(state, level,
    // pos, false). Used by the worldgen shape-update pass after tree placement.
    BlockState* getUpdatedStateForShapeUpdate(
        BlockState* state,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const {
        return getUpdatedState(state, level, pos, false);
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(BASE, NORTH, EAST, SOUTH, WEST);
    }

private:
    static constexpr std::array<core::Direction, 4> kHorizontalDirections = {
        core::Direction::NORTH,
        core::Direction::EAST,
        core::Direction::SOUTH,
        core::Direction::WEST
    };

    static void initializeProperties() {
        if (!BASE) {
            BlockStateProperties::initialize();
            // Java: MossyCarpetBlock.BASE = BlockStateProperties.BOTTOM ("bottom")
            BASE_STORAGE = BooleanProperty::create("bottom");
            BASE = BASE_STORAGE.get();
            NORTH = BlockStateProperties::NORTH_WALL;
            EAST = BlockStateProperties::EAST_WALL;
            SOUTH = BlockStateProperties::SOUTH_WALL;
            WEST = BlockStateProperties::WEST_WALL;
        }
    }

    static EnumProperty<WallSide>* getPropertyForFace(core::Direction direction) {
        initializeProperties();
        switch (direction) {
            case core::Direction::NORTH: return NORTH;
            case core::Direction::EAST: return EAST;
            case core::Direction::SOUTH: return SOUTH;
            case core::Direction::WEST: return WEST;
            default: return nullptr;
        }
    }

public:
    static bool hasFaces(BlockState* state) {
        if (!state) {
            return false;
        }

        if (state->getValueOrElse(*BASE, false)) {
            return true;
        }

        for (core::Direction direction : kHorizontalDirections) {
            EnumProperty<WallSide>* property = getPropertyForFace(direction);
            if (property && state->getValue(*property) != WallSide::none()) {
                return true;
            }
        }

        return false;
    }

    static bool canSupportAtFace(
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        core::Direction direction
    ) {
        return direction != core::Direction::UP && MultifaceBlock::canAttachTo(level, pos, direction);
    }

    BlockState* getUpdatedState(
        BlockState* state,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        bool createSides
    ) const {
        if (!state) {
            return nullptr;
        }

        BlockState* aboveState = nullptr;
        BlockState* belowState = nullptr;
        createSides |= state->getValueOrElse(*BASE, false);

        for (core::Direction direction : kHorizontalDirections) {
            EnumProperty<WallSide>* property = getPropertyForFace(direction);
            if (!property) {
                continue;
            }

            WallSide side = canSupportAtFace(level, pos, direction)
                ? (createSides ? WallSide::low() : state->getValue(*property))
                : WallSide::none();

            if (side == WallSide::low()) {
                if (!aboveState) {
                    aboveState = level.getBlockState(pos.above());
                }

                if (aboveState &&
                    aboveState->is(this) &&
                    !aboveState->getValueOrElse(*BASE, false) &&
                    aboveState->getValue(*property) != WallSide::none()) {
                    side = WallSide::tall();
                }

                if (!state->getValueOrElse(*BASE, false)) {
                    if (!belowState) {
                        belowState = level.getBlockState(pos.below());
                    }

                    if (belowState &&
                        belowState->is(this) &&
                        belowState->getValue(*property) == WallSide::none()) {
                        side = WallSide::none();
                    }
                }
            }

            state = state->setValue(*property, side);
        }

        return state;
    }

    BlockState* createTopperWithSideChance(
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos,
        minecraft::XoroshiroRandomSource& random
    ) const {
        const core::BlockPos abovePos = pos.above();
        BlockState* abovePreviousState = level.getBlockState(abovePos);
        bool isMossyCarpetAbove = abovePreviousState && abovePreviousState->is(this);

        if ((isMossyCarpetAbove && abovePreviousState->getValueOrElse(*BASE, false)) ||
            (!isMossyCarpetAbove && abovePreviousState && !abovePreviousState->canBeReplaced())) {
            if (std::FILE* ct = carpetTraceFile()) {
                std::fprintf(ct, "CT SKIP %d,%d,%d above=%s\n",
                             pos.getX(), pos.getY(), pos.getZ(),
                             abovePreviousState ? abovePreviousState->toStateString().c_str() : "null");
            }
            return nullptr;
        }
        if (std::FILE* ct = carpetTraceFile()) {
            std::fprintf(ct, "CT TOPPER %d,%d,%d above=%s\n",
                         pos.getX(), pos.getY(), pos.getZ(),
                         abovePreviousState ? abovePreviousState->toStateString().c_str() : "null");
        }

        BlockState* noBaseState = defaultBlockState()->setValue(*BASE, false);
        BlockState* aboveState = getUpdatedState(noBaseState, level, abovePos, true);
        if (!aboveState) {
            return nullptr;
        }

        for (core::Direction direction : kHorizontalDirections) {
            EnumProperty<WallSide>* property = getPropertyForFace(direction);
            if (property && aboveState->getValue(*property) != WallSide::none()) {
                bool keep = random.nextBoolean();
                if (std::FILE* trace = minecraft::levelgen::WorldgenRandom::s_rngTraceFile) {
                    std::fprintf(trace, "# CARPET side dir=%d keep=%d at %d,%d,%d\n",
                                 static_cast<int>(direction), keep ? 1 : 0,
                                 pos.getX(), pos.getY(), pos.getZ());
                }
                if (std::FILE* ct = carpetTraceFile()) {
                    std::fprintf(ct, "CT SIDE %d,%d,%d dir=%d keep=%d\n",
                                 pos.getX(), pos.getY(), pos.getZ(),
                                 static_cast<int>(direction), keep ? 1 : 0);
                }
                if (!keep) {
                    aboveState = aboveState->setValue(*property, WallSide::none());
                }
            }
        }

        if (hasFaces(aboveState) && aboveState != abovePreviousState) {
            return aboveState;
        }

        return nullptr;
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
