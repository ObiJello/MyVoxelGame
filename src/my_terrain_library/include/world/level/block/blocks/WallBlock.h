#pragma once

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
using state::properties::EnumProperty;
using state::properties::WallSide;

/**
 * WallBlock - Block with wall connections
 * Reference: net/minecraft/world/level/block/WallBlock.java
 *
 * Has properties: UP, EAST_WALL, NORTH_WALL, SOUTH_WALL, WEST_WALL, WATERLOGGED
 * Creates 2 * 3^4 * 2 = 324 states
 */
class WallBlock : public Block {
public:
    static inline BooleanProperty* UP = nullptr;
    static inline EnumProperty<WallSide>* EAST_WALL = nullptr;
    static inline EnumProperty<WallSide>* NORTH_WALL = nullptr;
    static inline EnumProperty<WallSide>* SOUTH_WALL = nullptr;
    static inline EnumProperty<WallSide>* WEST_WALL = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    explicit WallBlock(const Properties& properties) : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*UP, true);
            defaultState = defaultState->setValue(*EAST_WALL, WallSide::none());
            defaultState = defaultState->setValue(*NORTH_WALL, WallSide::none());
            defaultState = defaultState->setValue(*SOUTH_WALL, WallSide::none());
            defaultState = defaultState->setValue(*WEST_WALL, WallSide::none());
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(UP, EAST_WALL, NORTH_WALL, SOUTH_WALL, WEST_WALL, WATERLOGGED);
    }

private:
    static void initializeProperties() {
        if (!UP) {
            BlockStateProperties::initialize();
            UP = BlockStateProperties::UP;
            EAST_WALL = BlockStateProperties::EAST_WALL;
            NORTH_WALL = BlockStateProperties::NORTH_WALL;
            SOUTH_WALL = BlockStateProperties::SOUTH_WALL;
            WEST_WALL = BlockStateProperties::WEST_WALL;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
