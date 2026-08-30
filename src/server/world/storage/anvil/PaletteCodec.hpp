// File: src/server/world/storage/anvil/PaletteCodec.hpp
//
// The disk encoding of a PalettedContainer — MC's PalettedContainer.pack()
// and its inverse.
//
// WHY A REPACK IS MANDATORY. The in-memory container and the on-disk form
// are not the same thing, and dumping RawWords() is wrong in three separate
// ways:
//
//   1. A container that overflowed its tiers is GLOBAL: RawWords() then holds
//      raw VALUES, not palette indices, and Palette() is empty. On disk there
//      is no such thing — vanilla's pack() always writes a local palette and
//      local indices, at ceillog2(paletteSize) bits. The global palette is an
//      in-memory representation only.
//   2. The engine's PaletteStrategy carries a single globalBits, so
//      StorageBits() for block states is one of 0/4/5/6/7/8/15 and never 9..14
//      — exactly the widths a disk palette of 257..16384 entries needs.
//   3. IdFor/Grow are append-only, so a palette accumulates entries no voxel
//      references any more after edits. Vanilla's pack() rebuilds from a fresh
//      map and drops them; keeping them would inflate the width and, past 256
//      entries, change it.
//
// THE PACKING ITSELF (SimpleBitStorage): entries NEVER straddle a long.
// valuesPerLong = 64 / bits (integer division), so the top
// 64 - bits*valuesPerLong bits of every long are zero padding. Entry i sits in
// data[i / valuesPerLong] at bit offset (i % valuesPerLong) * bits, least
// significant entry first. data.length must be EXACTLY
// ceil(entryCount / valuesPerLong) — a mismatch throws in vanilla and takes
// down the whole chunk, not just the section.
//
// Deliberately depends on nothing but PalettedContainer: no block registry, no
// biome registry. That keeps it testable against vanilla's own bytes without
// any id mapping in the way.
#pragma once

#include "common/world/chunk/PalettedContainer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Game::Anvil {

    // What one `block_states` or `biomes` compound holds on disk.
    struct DiskContainer {
        std::vector<uint32_t> palette;   // values, in the engine's id space
        std::vector<uint64_t> data;      // EMPTY when bits == 0 -> omit the key
        int                   bits = 0;
    };

    // MC Strategy.getConfigurationForPaletteSize -> Configuration.bitsInStorage.
    //
    //   blocks: 1 -> 0 (no data array); 2..16 -> 4; 17..32 -> 5; 33..64 -> 6;
    //           65..128 -> 7; 129..256 -> 8; above -> ceillog2(n)
    //   biomes: 1 -> 0; 2 -> 1; 3..4 -> 2; 5..8 -> 3; above -> ceillog2(n)
    //
    // NOTE the >8 case returns ceillog2(n), NOT the global-palette width. This
    // is the one place the engine's own WidthFor deliberately disagrees with
    // the disk form.
    int DiskBitsFor(size_t paletteSize, bool blockStateTiers);

    // Words a packed array must contain. Exact, not a bound.
    inline size_t DiskWordCount(size_t entryCount, int bits) {
        if (bits <= 0) return 0;
        const size_t perLong = static_cast<size_t>(64 / bits);
        return (entryCount + perLong - 1) / perLong;
    }

    // ── Low level, index in / index out ─────────────────────────────────────
    // Separated from the container so both directions can be checked against
    // a file vanilla wrote without any value mapping in between.

    void PackIndices(const std::vector<uint32_t>& indices, int bits,
                     std::vector<uint64_t>& out);

    // False when `data` is not exactly DiskWordCount(entryCount, bits) long.
    bool UnpackIndices(const std::vector<uint64_t>& data, size_t entryCount, int bits,
                       std::vector<uint32_t>& out);

    // ── Container level ─────────────────────────────────────────────────────

    // Rebuild a local palette from the container's ACTUAL contents, in
    // first-seen order (which is what reencodeContents produces), and pack at
    // the disk width.
    DiskContainer PackForDisk(const PalettedContainer& container);

    // Inverse. `strategy` is the engine-side configuration to build into, so
    // the container comes back in the engine's own tiering rather than the
    // disk one. Returns false with `error` set on any inconsistency — a wrong
    // word count, a palette index out of range, or a non-trivial palette with
    // no data array ("Missing values for non-zero storage" in vanilla).
    bool UnpackFromDisk(const std::vector<uint32_t>& palette,
                        const std::vector<uint64_t>& data,
                        const PaletteStrategy& strategy,
                        PalettedContainer& out,
                        std::string& error);

} // namespace Game::Anvil
