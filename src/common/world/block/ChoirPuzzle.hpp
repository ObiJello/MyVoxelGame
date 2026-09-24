// File: src/common/world/block/ChoirPuzzle.hpp
//
// The Choir Hall's puzzle (docs/the-hush.md, "The Choir Hall"): the two
// blocks and the song that summons the Choir Mother.
//
//   resonant_chime — right-click to strike it: it sounds (the
//                    obeycraft:block.resonant_chime.strike event) and lights for 12 ticks via
//                    its `lit` state; lit, its animateTick throws motes.
//   choir_altar    — right-click with an echo shard to start the song. The
//                    altar takes the shard and "sings": it lights the ring's
//                    chimes one at a time in a random sequence of 4-6. The
//                    player then strikes the chimes in the same order within
//                    5 + 2 seconds a note. Right: the shard is spent and the
//                    Choir Mother rises three blocks above the altar. Wrong,
//                    or out of time: the song resets and the shard is
//                    returned on the altar.
//
// THE RING. Every resonant_chime within 12 blocks horizontally and 4
// vertically of the altar, ordered clockwise from north by its bearing from
// the altar (ties nearest first) — so a structure only has to place chimes
// around an altar; there is no linking step. At least three are needed.
//
// The song state is server-only and lives in this module (keyed by
// dimension and altar), driven by the altar's scheduled block ticks.
#pragma once

#include "common/world/block/BlockRegistry.hpp"

#include <array>

namespace Game {

    // Wires the chime's and the altar's behaviours and marks both emissive.
    // Called from BlockRegistry::Init after InitBlockStates (the chime reads
    // its `lit` property).
    void BlockRegistry_RegisterChoirBlocks(std::array<Block, BlockRegistry::Size>& blocks);

    namespace ChoirPuzzle {
        inline constexpr int kRingRadius      = 12;   // horizontal, blocks
        inline constexpr int kRingHeight      = 4;    // vertical, each way
        inline constexpr int kMinChimes       = 3;
        inline constexpr int kChimeLitTicks   = 12;
        inline constexpr int kNoteInterval    = 16;   // ticks between sung notes
        inline constexpr int kLeadInTicks     = 20;
        inline constexpr double kMotherLift   = 3.0;  // blocks above the altar
    }

} // namespace Game
