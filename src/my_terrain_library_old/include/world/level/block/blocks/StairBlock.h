#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "core/Direction.h"

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
using state::properties::Half;
using state::properties::StairsShape;

/**
 * StairBlock - Block with stair shape
 * Reference: net/minecraft/world/level/block/StairBlock.java
 *
 * Has properties: FACING, HALF, SHAPE, WATERLOGGED
 * Creates 4 * 2 * 5 * 2 = 80 states
 */
class StairBlock : public Block {
public:
    // Static property references
    // Reference: StairBlock.java lines 32-35
    static inline DirectionProperty* FACING = nullptr;
    static inline EnumProperty<Half>* HALF = nullptr;
    static inline EnumProperty<StairsShape>* SHAPE = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    /**
     * Constructor
     * Reference: StairBlock.java lines 52-57
     */
    explicit StairBlock(const Properties& properties) : Block(properties) {
        // Initialize static properties if needed
        initializeProperties();

        // Set default state
        // Reference: StairBlock.java line 54
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*HALF, Half::bottom());
            defaultState = defaultState->setValue(*SHAPE, StairsShape::straight());
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    /**
     * Create block state definition
     * Reference: StairBlock.java createBlockStateDefinition() lines 215-217
     */
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(FACING, HALF, SHAPE, WATERLOGGED);
    }

private:
    static void initializeProperties() {
        if (!FACING) {
            BlockStateProperties::initialize();
            FACING = BlockStateProperties::HORIZONTAL_FACING;
            HALF = BlockStateProperties::HALF;
            SHAPE = BlockStateProperties::STAIRS_SHAPE;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
