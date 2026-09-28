// File: src/common/world/block/JukeboxBlock.hpp
//
// Mirrors net.minecraft.world.level.block.JukeboxBlock (with
// JukeboxPlayable.tryInsertIntoJukebox) — the block half of the jukebox:
// putting a disc in, taking it out, the redstone signal while a song plays
// and the comparator reading of the disc. The disc and the song live in
// JukeboxBlockEntity.
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    // Wires useItemOn / useWithoutItem / the signal hooks /
    // affectNeighborsAfterRemoval onto BlockID::Jukebox. Called from
    // RegisterBlockBehaviors after the container wiring.
    void RegisterJukeboxBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
