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
using state::properties::SlabType;

/**
 * SlabBlock - Half-height block
 * Reference: net/minecraft/world/level/block/SlabBlock.java
 *
 * Has properties: TYPE, WATERLOGGED
 * Creates 3 * 2 = 6 states
 */
class SlabBlock : public Block {
public:
    // Static property references
    // Reference: SlabBlock.java lines 33-34
    static inline EnumProperty<SlabType>* TYPE = nullptr;
    static inline BooleanProperty* WATERLOGGED = nullptr;

    /**
     * Constructor
     * Reference: SlabBlock.java lines 42-45
     */
    explicit SlabBlock(const Properties& properties) : Block(properties) {
        initializeProperties();

        // Set default state
        // Reference: SlabBlock.java line 44
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*TYPE, SlabType::bottom());
            defaultState = defaultState->setValue(*WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    /**
     * Create block state definition
     * Reference: SlabBlock.java createBlockStateDefinition() lines 51-53
     */
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(TYPE, WATERLOGGED);
    }

private:
    static void initializeProperties() {
        if (!TYPE) {
            BlockStateProperties::initialize();
            TYPE = BlockStateProperties::SLAB_TYPE;
            WATERLOGGED = BlockStateProperties::WATERLOGGED;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
