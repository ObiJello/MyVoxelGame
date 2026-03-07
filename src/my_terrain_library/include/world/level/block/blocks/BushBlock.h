#pragma once

#include "levelgen/WorldGenLevel.h"
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
        if (!stateBelow) {
            return false;
        }

        const std::string& name = stateBelow->getIdentifier();
        return name == "minecraft:dirt" ||
               name == "minecraft:grass_block" ||
               name == "minecraft:podzol" ||
               name == "minecraft:coarse_dirt" ||
               name == "minecraft:mycelium" ||
               name == "minecraft:rooted_dirt" ||
               name == "minecraft:mud" ||
               name == "minecraft:muddy_mangrove_roots" ||
               name == "minecraft:moss_block" ||
               name == "minecraft:farmland";
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
