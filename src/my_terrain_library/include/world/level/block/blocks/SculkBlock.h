#pragma once

#include "world/level/block/Block.h"
#include "world/level/block/SculkSpreader.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;

class SculkBlock : public Block, public SculkBehaviour {
public:
    explicit SculkBlock(const Properties& properties);

    int attemptUseCharge(ChargeCursor& cursor, levelgen::WorldGenLevel* level,
                         const core::BlockPos& originPos, minecraft::levelgen::WorldgenRandom& random,
                         SculkSpreader& spreader, bool spreadVeins) const override;

    bool canChangeBlockStateOnSpread() const override;

private:
    static int getDecayPenalty(const SculkSpreader& spreader, const core::BlockPos& pos,
                               const core::BlockPos& originPos, int charge);
    BlockState* getRandomGrowthState(levelgen::WorldGenLevel* level, const core::BlockPos& pos,
                                     minecraft::levelgen::WorldgenRandom& random, bool isWorldGen) const;
    static bool canPlaceGrowth(levelgen::WorldGenLevel* level, const core::BlockPos& pos);
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
