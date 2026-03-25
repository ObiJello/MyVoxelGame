#pragma once

#include "world/level/block/blocks/GrowingPlantBodyBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::BooleanProperty;

class CaveVinesPlantBlock : public GrowingPlantBodyBlock {
public:
    static inline BooleanProperty* BERRIES = nullptr;

    explicit CaveVinesPlantBlock(const Properties& properties)
        : GrowingPlantBodyBlock(Properties(properties).noCollission(), core::Direction::DOWN, false) {
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

    bool isHeadOrBody(BlockState* state) const override {
        if (!state) {
            return false;
        }
        const std::string& id = state->getIdentifier();
        return id == "minecraft:cave_vines" || id == "minecraft:cave_vines_plant";
    }

    BlockState* updateHeadAfterConvertedFromBody(
        BlockState* bodyState,
        BlockState* headState
    ) const override {
        if (!bodyState || !headState) {
            return headState;
        }
        return headState->setValue(*BERRIES, bodyState->getValue(*BERRIES));
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
