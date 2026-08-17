// File: src/common/world/block/BlockStateIds.hpp
//
// Split out of BlockRegistry.hpp so the storage layer can depend on the id
// space without dragging in the whole block registry — ChunkSection needs
// Pack/Unpack/Bits and nothing else, and including BlockRegistry.hpp there also
// put Game::BlockRegistry in scope everywhere, shadowing the terrain library's
// identically-named class.
#pragma once

#include "Blocks.hpp"
#include <cstdint>

namespace Game {

    // ── Flat block-state id space (MC Block.BLOCK_STATE_REGISTRY) ───────────
    //
    // MC gives every (block, state) pair one dense integer, assigned by walking
    // blocks in registry order and, within each, its possible states:
    //
    //     for (Block block : BuiltInRegistries.BLOCK)
    //        for (BlockState state : block.getStateDefinition().getPossibleStates())
    //           Block.BLOCK_STATE_REGISTRY.add(state);          // Blocks.java:2461
    //
    // `base[block] + stateIndex` is exactly that ordering, and the table it
    // needs was already being built for the shape cache — this shares it rather
    // than building a second one.
    //
    // Dense is the point: ~1300 distinct states across ~1166 blocks fit in 11
    // bits, where the (BlockID, stateIndex) pair needs 19 with most of the
    // space empty. That difference is what a palette's global fallback pays per
    // voxel.
    //
    // THESE IDS ARE RUNTIME-DERIVED AND MUST NEVER BE PERSISTED. MC's are not
    // either — Anvil stores names and properties and rebuilds the mapping on
    // load. Ours is a pure function of BlockID order and each block's declared
    // property list, so a client and server on the same build agree by
    // construction, and a save file would survive neither of them changing.
    namespace BlockStateIds {

        // (block, state) -> flat id. `stateIndex` is clamped to the block's
        // state count, so an out-of-range index degrades to that block's last
        // state rather than reading into the next block's slice.
        uint32_t Pack(BlockID id, uint8_t stateIndex);

        // flat id -> (block, state). Out-of-range answers Air's default state.
        BlockStateRef Unpack(uint32_t stateId);

        // Total distinct states, and the bit width a global palette needs for
        // them (MC Strategy.globalPaletteBitsInMemory = ceillog2(registry size)).
        uint32_t Count();
        int      Bits();

    } // namespace BlockStateIds

} // namespace Game
