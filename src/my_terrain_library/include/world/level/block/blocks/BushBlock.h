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
        return mayPlaceOn(level.getBlockState(pos.below()), level, pos.below());
    }

protected:
    // VegetationBlock.mayPlaceOn(state, level, pos): the level-aware form
    // (seagrass tests the face below); plain plants only need the state.
    virtual bool mayPlaceOn(BlockState* stateBelow, const levelgen::WorldGenLevel& /*level*/,
                            const core::BlockPos& /*belowPos*/) const {
        return mayPlaceOn(stateBelow);
    }

    virtual bool mayPlaceOn(BlockState* stateBelow) const {
        // 26.3 VegetationBlock.mayPlaceOn: BlockTags.SUPPORTS_VEGETATION
        return minecraft::levelgen::blockpredicates::matchesBlockTagName(stateBelow, "minecraft:supports_vegetation");
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
