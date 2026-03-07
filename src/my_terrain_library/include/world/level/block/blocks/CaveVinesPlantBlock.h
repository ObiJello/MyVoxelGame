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

class CaveVinesPlantBlock : public Block {
public:
    static inline BooleanProperty* BERRIES = nullptr;

    explicit CaveVinesPlantBlock(const Properties& properties)
        : Block(Properties(properties).noCollission().replaceable()) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BERRIES, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(BERRIES);
    }

private:
    static void initializeProperties() {
        if (!BERRIES) {
            BlockStateProperties::initialize();
            BERRIES = BlockStateProperties::BERRIES;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
