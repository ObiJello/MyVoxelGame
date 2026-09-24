// File: src/common/world/block/RedstoneContainers.hpp
//
// Hopper / dispenser / dropper block hooks and the comparator readings of
// every container. See the .cpp.
#pragma once

#include "common/world/block/BlockRegistry.hpp"

#include <array>

namespace Game {

    void RegisterContainerBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
