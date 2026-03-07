#pragma once

#include "world/level/block/blocks/BushBlock.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

class EyeblossomBlock : public BushBlock {
public:
    explicit EyeblossomBlock(bool open, const Properties& properties)
        : BushBlock(properties)
        , m_open(open) {}

    bool isOpen() const { return m_open; }

private:
    bool m_open;
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
