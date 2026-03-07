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
using state::properties::IntegerProperty;

class CaveVinesBlock : public Block {
public:
    static inline IntegerProperty* AGE = nullptr;
    static inline BooleanProperty* BERRIES = nullptr;

    explicit CaveVinesBlock(const Properties& properties)
        : Block(Properties(properties).noCollission().replaceable()) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*AGE, 0);
            defaultState = defaultState->setValue(*BERRIES, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(AGE, BERRIES);
    }

private:
    static void initializeProperties() {
        if (!AGE) {
            BlockStateProperties::initialize();
            AGE = BlockStateProperties::AGE_25;
            BERRIES = BlockStateProperties::BERRIES;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
