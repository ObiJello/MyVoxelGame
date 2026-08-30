// File: src/common/world/block/RedstoneSignal.cpp
#include "common/world/block/RedstoneSignal.hpp"

namespace Game {

    bool HasNeighborSignal(const IBlockAccess& level, const glm::ivec3& pos) {
        (void)level;
        (void)pos;
        // No redstone power simulation exists. See RedstoneSignal.hpp — this is
        // a deliberate seam, and every caller is already written against it.
        return false;
    }

} // namespace Game
