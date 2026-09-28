// File: src/common/world/block/DecoratedPotBlock.hpp
//
// Mirrors net.minecraft.world.level.block.DecoratedPotBlock's interaction:
// useItemOn puts one of the held stack into the pot (or onto the matching
// stack already there) with a POSITIVE wobble, the insert sound pitched by
// how full it is and a dust plume; anything else — an empty hand, a full
// pot, a different item — is useWithoutItem's NEGATIVE wobble and the
// insert-fail sound. The pot's contents, sherds and loot table live in
// DecoratedPotBlockEntity; its comparator reading and the spill when it
// breaks are the container wiring's (RedstoneContainers.cpp,
// PlayerSession's break).
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    // Wires useItemOn / useWithoutItem onto BlockID::DecoratedPot. Called
    // from RegisterBlockBehaviors.
    void RegisterDecoratedPotBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
