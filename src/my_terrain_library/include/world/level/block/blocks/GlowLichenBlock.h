#pragma once

#include "world/level/block/blocks/MultifaceSpreadeableBlock.h"
#include "world/level/block/blocks/MultifaceSpreader.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class GlowLichenBlock : public MultifaceSpreadeableBlock {
public:
    explicit GlowLichenBlock(const Properties& properties)
        : MultifaceSpreadeableBlock(properties)
        , m_spreader(this) {}

    const MultifaceSpreader& getSpreader() const override {
        return m_spreader;
    }

private:
    MultifaceSpreader m_spreader;
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
