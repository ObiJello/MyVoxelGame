#pragma once

#include "world/level/block/blocks/GrowingPlantBlock.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class GrowingPlantBodyBlock : public GrowingPlantBlock {
public:
    GrowingPlantBodyBlock(
        const Properties& properties,
        core::Direction growthDirection,
        bool scheduleFluidTicks
    )
        : GrowingPlantBlock(properties, growthDirection, scheduleFluidTicks)
    {}

protected:
    virtual BlockState* updateHeadAfterConvertedFromBody(
        BlockState* /*bodyState*/,
        BlockState* headState
    ) const {
        return headState;
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
