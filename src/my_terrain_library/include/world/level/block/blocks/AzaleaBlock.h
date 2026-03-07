#pragma once

#include "world/level/block/blocks/BushBlock.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class AzaleaBlock : public BushBlock {
public:
    explicit AzaleaBlock(const Properties& properties)
        : BushBlock(properties) {}

protected:
    bool mayPlaceOn(BlockState* stateBelow) const override {
        return stateBelow &&
               (stateBelow->getIdentifier() == "minecraft:clay" || BushBlock::mayPlaceOn(stateBelow));
    }
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
