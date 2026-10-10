// File: src/common/world/block/ColoredTorches.hpp
//
// The 16 dyed torches (engine, 2026-10-10): `<colour>_torch` and
// `<colour>_wall_torch` for every dye, appended to BlockDefs.inc as
// standing / wall pairs in DyeColor order (white, orange, magenta,
// light_blue, yellow, lime, pink, gray, light_gray, cyan, purple, blue,
// brown, green, red, black) — so membership, colour and the wall twin are
// arithmetic on the BlockID. Vanilla's torch in every way but colour: their
// light (BlockLightColor.inc) and their flame (BlockAnimateParticles).
#pragma once

#include "Blocks.hpp"

#include <cstdint>

namespace Game::ColoredTorches {

    inline constexpr int kCount = 16;

    inline constexpr int Offset(BlockID id) {
        return static_cast<int>(id) - static_cast<int>(BlockID::WhiteTorch);
    }

    // Any dyed torch, standing or on a wall.
    inline constexpr bool Is(BlockID id) {
        const int o = Offset(id);
        return o >= 0 && o < 2 * kCount;
    }
    inline constexpr bool IsStanding(BlockID id) { return Is(id) && (Offset(id) & 1) == 0; }
    inline constexpr bool IsWall(BlockID id)     { return Is(id) && (Offset(id) & 1) == 1; }

    // DyeColor ordinal, 0..15; -1 for any other block.
    inline constexpr int ColorOf(BlockID id) { return Is(id) ? Offset(id) / 2 : -1; }

    inline constexpr BlockID Standing(int color) {
        return static_cast<BlockID>(static_cast<int>(BlockID::WhiteTorch) + 2 * color);
    }
    inline constexpr BlockID Wall(int color) {
        return static_cast<BlockID>(static_cast<int>(BlockID::WhiteTorch) + 2 * color + 1);
    }

    // The colour of each torch's light and flame, 0xRRGGBB — saturated
    // takes on the dye colours, so the light reads as its dye even under a
    // warm lightmap (BlockLightColor.inc carries the same values; the light
    // is scaled so its brightest channel is the torch's level 14). Gray and
    // light gray are cool whites; black is a "black light" violet.
    inline constexpr uint32_t kColor[kCount] = {
        0xFFFFFF,   // white
        0xFF8A1F,   // orange
        0xFF3DF0,   // magenta
        0x5CC8FF,   // light blue
        0xFFE23A,   // yellow
        0x7DFF2A,   // lime
        0xFF7FB6,   // pink
        0xA9B3C2,   // gray
        0xD5DCE6,   // light gray
        0x1FE6E6,   // cyan
        0xA040FF,   // purple
        0x3050FF,   // blue
        0xD08848,   // brown
        0x2ED12E,   // green
        0xFF2A1A,   // red
        0x6A3CFF,   // black
    };

    static_assert(static_cast<int>(BlockID::BlackWallTorch) - static_cast<int>(BlockID::WhiteTorch) == 2 * kCount - 1,
                  "the dyed torches must stay 16 contiguous standing / wall pairs in BlockDefs.inc");

} // namespace Game::ColoredTorches
