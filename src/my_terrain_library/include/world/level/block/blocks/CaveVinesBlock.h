#pragma once

#include "world/level/block/blocks/GrowingPlantHeadBlock.h"
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

class CaveVinesBlock : public GrowingPlantHeadBlock {
public:
    static inline IntegerProperty* AGE = nullptr;
    static inline BooleanProperty* BERRIES = nullptr;

    explicit CaveVinesBlock(const Properties& properties)
        : GrowingPlantHeadBlock(Properties(properties).noCollission(), core::Direction::DOWN, false, 0.1) {
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
        GrowingPlantHeadBlock::createBlockStateDefinition(builder);
        builder.add(BERRIES);
    }

    int getBlocksToGrowWhenBonemealed(minecraft::levelgen::WorldgenRandom& /*random*/) const override {
        return 1;
    }

    bool canGrowInto(BlockState* state) const override {
        return state && state->isAir();
    }

    bool isHeadOrBody(BlockState* state) const override {
        if (!state) {
            return false;
        }
        const std::string& id = state->getIdentifier();
        return id == "minecraft:cave_vines" || id == "minecraft:cave_vines_plant";
    }

    BlockState* updateBodyAfterConvertedFromHead(
        BlockState* headState,
        BlockState* bodyState
    ) const override {
        if (!headState || !bodyState) {
            return bodyState;
        }
        return bodyState->setValue(*BERRIES, headState->getValue(*BERRIES));
    }

private:
    static void initializeProperties() {
        if (!AGE) {
            BlockStateProperties::initialize();
            AGE = GrowingPlantHeadBlock::AGE;
            BERRIES = BlockStateProperties::BERRIES;
        }
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
