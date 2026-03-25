#pragma once

#include "core/Direction.h"
#include "levelgen/WorldGenLevel.h"
#include "world/level/block/Block.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class GrowingPlantBlock : public Block {
public:
    GrowingPlantBlock(
        const Properties& properties,
        core::Direction growthDirection,
        bool scheduleFluidTicks
    )
        : Block(properties)
        , m_growthDirection(growthDirection)
        , m_scheduleFluidTicks(scheduleFluidTicks)
    {}

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        const core::BlockPos attachedToPos = pos.relative(core::getOpposite(m_growthDirection));
        BlockState* attachedToState = level.getBlockState(attachedToPos);
        if (!canAttachTo(attachedToState)) {
            return false;
        }

        return (attachedToState && isHeadOrBody(attachedToState)) ||
               (attachedToState && attachedToState->isFaceSturdy(level, attachedToPos, m_growthDirection));
    }

protected:
    virtual bool canAttachTo(BlockState* /*state*/) const {
        return true;
    }

    virtual bool isHeadOrBody(BlockState* state) const = 0;

    core::Direction growthDirection() const {
        return m_growthDirection;
    }

    bool scheduleFluidTicks() const {
        return m_scheduleFluidTicks;
    }

private:
    core::Direction m_growthDirection;
    bool m_scheduleFluidTicks;
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
