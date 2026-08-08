#pragma once

#include "levelgen/WorldGenLevel.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/level/block/Block.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class BushBlock : public Block {
public:
    explicit BushBlock(const Properties& properties)
        : Block(Properties(properties).noCollission().replaceableByTrees()) {}

    bool canSurvive(
        BlockState* /*state*/,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        return mayPlaceOn(level.getBlockState(pos.below()));
    }

protected:
    virtual bool mayPlaceOn(BlockState* stateBelow) const {
        return minecraft::levelgen::blockpredicates::matchesBlockTagName(stateBelow, "minecraft:dirt") ||
               (stateBelow && stateBelow->getIdentifier() == "minecraft:farmland");
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
