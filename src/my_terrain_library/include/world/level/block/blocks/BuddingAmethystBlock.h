#pragma once

#include "world/level/block/Block.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

using state::BlockState;

class BuddingAmethystBlock : public Block {
public:
    explicit BuddingAmethystBlock(const Properties& properties)
        : Block(properties) {}

    static bool canClusterGrowAtState(BlockState* state) {
        return state && (state->isAir() || state->getIdentifier() == "minecraft:water");
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
