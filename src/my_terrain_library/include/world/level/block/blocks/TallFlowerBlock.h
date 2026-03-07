#pragma once

#include "world/level/block/blocks/DoublePlantBlock.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class TallFlowerBlock : public DoublePlantBlock {
public:
    explicit TallFlowerBlock(const Properties& properties)
        : DoublePlantBlock(properties) {}
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
