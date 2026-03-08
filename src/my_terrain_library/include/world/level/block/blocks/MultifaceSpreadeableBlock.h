#pragma once

#include "world/level/block/blocks/MultifaceBlock.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class MultifaceSpreader;

class MultifaceSpreadeableBlock : public MultifaceBlock {
public:
    explicit MultifaceSpreadeableBlock(const Properties& properties)
        : MultifaceBlock(properties) {}

    virtual const MultifaceSpreader& getSpreader() const = 0;
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
