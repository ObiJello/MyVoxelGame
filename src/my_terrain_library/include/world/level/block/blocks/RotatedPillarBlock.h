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
using state::properties::AxisProperty;
using core::Axis;

/**
 * RotatedPillarBlock - Block that can be rotated on placement (logs, pillars)
 * Reference: net/minecraft/world/level/block/RotatedPillarBlock.java
 *
 * Has properties: AXIS
 * Creates 3 states (X, Y, Z)
 */
class RotatedPillarBlock : public Block {
public:
    static inline AxisProperty* AXIS = nullptr;

    explicit RotatedPillarBlock(const Properties& properties) : Block(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*AXIS, Axis::Y);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(AXIS);
    }

private:
    static void initializeProperties() {
        if (!AXIS) {
            BlockStateProperties::initialize();
            AXIS = BlockStateProperties::AXIS;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
