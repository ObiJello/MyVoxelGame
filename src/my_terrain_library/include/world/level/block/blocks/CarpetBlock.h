#pragma once

#include "levelgen/WorldGenLevel.h"
#include "world/level/block/Block.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class CarpetBlock : public Block {
public:
    explicit CarpetBlock(const Properties& properties)
        : Block(Properties(properties).noCollission()) {}

    bool canSurvive(
        BlockState* /*state*/,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        return !level.isEmptyBlock(pos.below());
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
