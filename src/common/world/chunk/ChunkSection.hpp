// File: src/common/world/chunk/ChunkSection.hpp
#pragma once

#include <vector>
#include <cstdint>
#include "../math/WorldMath.hpp"
#include "../block/Blocks.hpp"

namespace Game {

    // A single 16×16×16 sub‐chunk (also called a “section”).
    // Internally we store exactly 4096 block indices (one uint16_t per local voxel),
    // plus a palette (to be used later for PalettedStorage if desired).
    //
    // Memory layout: 
    //   - blocks[y][z][x] flattened via LocalIndex(x,y,z), where each runs 0..15.
    //
    // Get/Set methods do no bounds-checking in release; you may add asserts if you like.
    class ChunkSection {
    public:
        static constexpr int SIZE = 16;        // extent along each axis
        static constexpr int TOTAL = SIZE * SIZE * SIZE; // 4096

        // Each entry is a uint16_t that holds a BlockID value (0..Count-1).
        // Default‐initialize to Air (BlockID::Air == 0).
        std::vector<uint16_t> blocks{ std::vector<uint16_t>(TOTAL, static_cast<uint16_t>(BlockID::Air)) };

        // Palette is reserved for later if we switch to a palette-based scheme.
        // For now, we won’t actually use it; it’s just a placeholder.
        std::vector<uint16_t> palette;

        // Per-voxel block-state index. This is an index into the OWNING BLOCK's
        // own state list (BlockRegistry::GetStateDefinition), i.e. exactly MC's
        // `BlockState.getId()` concept — NOT a bag of raw property bits. Index 0
        // is always the block's default state (MC `defaultBlockState()`), so a
        // zero byte is meaningful for every block and needs no interpretation.
        //
        // LAZY ON PURPOSE: an empty vector means "every voxel here is at its
        // default state", which is true for the overwhelming majority of
        // sections — terrain is stone/dirt/air, none of which have properties.
        // Allocating eagerly would add 4 KB per section; at render distance 32
        // that is hundreds of MB of almost entirely zero bytes. The vector is
        // materialised on the first non-default write and never shrinks back
        // (a section that had one furnace keeps its 4 KB, which is fine).
        std::vector<uint8_t> states;

        ChunkSection() = default;

        // State index at local (x,y,z). Returns 0 (the block's default state)
        // when this section has never had a non-default state written.
        inline uint8_t GetState(int x, int y, int z) const {
            if (states.empty()) return 0;
            return states[ static_cast<size_t>(Math::LocalIndex(x, y, z)) ];
        }

        inline void SetState(int x, int y, int z, uint8_t stateIndex) {
            if (states.empty()) {
                // Writing the default into an all-default section is a no-op —
                // don't pay 4 KB for it.
                if (stateIndex == 0) return;
                states.assign(TOTAL, 0);
            }
            states[ static_cast<size_t>(Math::LocalIndex(x, y, z)) ] = stateIndex;
        }

        // True when this section carries no per-voxel state at all. Lets the
        // chunk serialiser skip the state plane entirely for ordinary terrain.
        inline bool HasStates() const { return !states.empty(); }

        // Retrieve the BlockID at local (x,y,z) ∈ [0..15].
        // Returns a raw uint16_t; you can static_cast<BlockID>(…) when needed.
        inline uint16_t Get(int x, int y, int z) const {
            // (Optionally) add an assert in debug:
            // assert(x >= 0 && x < SIZE && y >= 0 && y < SIZE && z >= 0 && z < SIZE);
            return blocks[ static_cast<size_t>(Math::LocalIndex(x, y, z)) ];
        }

        // Overload to get as enum:
        inline BlockID GetBlockID(int x, int y, int z) const {
            return static_cast<BlockID>(Get(x, y, z));
        }

        // Set the block index at local (x,y,z). Provide a raw uint16_t or BlockID.
        inline void Set(int x, int y, int z, uint16_t rawID) {
            // assert(x >= 0 && x < SIZE && y >= 0 && y < SIZE && z >= 0 && z < SIZE);
            blocks[ static_cast<size_t>(Math::LocalIndex(x, y, z)) ] = rawID;
        }

        inline void Set(int x, int y, int z, BlockID id) {
            Set(x, y, z, static_cast<uint16_t>(id));
        }
    };

} // namespace Game
