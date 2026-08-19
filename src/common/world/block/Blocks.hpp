// File: src/common/world/block/Blocks.hpp
#pragma once
#include <cstdint>

namespace Game {

    // We'll use a uint16_t under the hood to match our palette/palette-index style.
    enum class BlockID : uint16_t {
        Air = 0,

        // All blocks defined in BlockDefs.inc (single source of truth)
        #define BLOCK_DEF(e, m, d, o) e,
        #include "BlockDefs.inc"
        #undef BLOCK_DEF

        // No manual entries remain. Every variant that used to live here —
        // *SlabTop/*SlabDouble, SnowGrass, the double-plant tops, BeeNestHoney
        // and the segmented clumps — was a BlockID standing in for ONE property
        // value. They are real blockstates now, exactly as in vanilla, so a
        // block's identity and its state are finally separate things.

        Count // Always keep this as the last entry.
    };

    // A state's index within its OWN block's state list — MC's
    // `BlockState.getId() - block.minStateId`.
    //
    // 16-bit, not 8: 30 vanilla blocks have more than 256 states (redstone_wire
    // 1296, note_block 1150, fire 512, calibrated_sculk_sensor 384, and every
    // wall at 324). A uint8_t could not address them, which is why walls
    // shipped without `waterlogged` and why StateIdTable silently clamped.
    //
    // Named rather than spelled out so the eventual swap to the BlockState
    // handle — which carries the block too, and drops the pair entirely — is a
    // change to one line plus the call sites that stop needing two arguments.
    using BlockStateIndex = uint16_t;

    // `BlockStateRef` — a BlockID paired with a within-block index — used to
    // live here, for the places the two must not get separated in transit
    // (change accumulation, prediction rollback, mesher lookup). It is gone:
    // BlockState (BlockState.hpp) is a single 32-bit value that carries both,
    // so there is nothing left to keep together by hand.

} // namespace Game
