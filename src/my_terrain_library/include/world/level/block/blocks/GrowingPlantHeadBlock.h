#pragma once

#include "levelgen/WorldgenRandom.h"
#include "world/level/block/blocks/GrowingPlantBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::IntegerProperty;

class GrowingPlantHeadBlock : public GrowingPlantBlock {
public:
    static inline IntegerProperty* AGE = nullptr;

    GrowingPlantHeadBlock(
        const Properties& properties,
        core::Direction growthDirection,
        bool scheduleFluidTicks,
        double growPerTickProbability
    )
        : GrowingPlantBlock(properties, growthDirection, scheduleFluidTicks)
        , m_growPerTickProbability(growPerTickProbability)
    {
        initializeProperties();
    }

    BlockState* getStateForPlacement(minecraft::levelgen::WorldgenRandom& random) const {
        BlockState* defaultState = defaultBlockState();
        return defaultState ? defaultState->setValue(*AGE, random.nextInt(25)) : nullptr;
    }

    BlockState* getMaxAgeState(BlockState* fromState) const {
        return fromState ? fromState->setValue(*AGE, 25) : nullptr;
    }

    bool isMaxAge(BlockState* state) const {
        return state && state->getValue(*AGE) == 25;
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(AGE);
    }

    virtual BlockState* updateBodyAfterConvertedFromHead(
        BlockState* /*headState*/,
        BlockState* bodyState
    ) const {
        return bodyState;
    }

    virtual int getBlocksToGrowWhenBonemealed(minecraft::levelgen::WorldgenRandom& random) const = 0;
    virtual bool canGrowInto(BlockState* state) const = 0;

private:
    static void initializeProperties() {
        if (!AGE) {
            BlockStateProperties::initialize();
            AGE = BlockStateProperties::AGE_25;
        }
    }

    double m_growPerTickProbability;
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
