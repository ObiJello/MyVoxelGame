#pragma once

#include "levelgen/WorldGenLevel.h"
#include "world/level/block/Block.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class SporeBlossomBlock : public Block {
public:
    explicit SporeBlossomBlock(const Properties& properties)
        : Block(properties) {}

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        const core::BlockPos abovePos = pos.above();
        BlockState* aboveState = level.getBlockState(abovePos);
        BlockState* currentState = level.getBlockState(pos);
        return aboveState &&
               aboveState->isCollisionShapeFullBlock(level, abovePos) &&
               (!currentState || currentState->getIdentifier() != "minecraft:water");
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
