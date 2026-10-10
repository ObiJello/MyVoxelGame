// File: src/common/world/lighting/LightCoords.hpp
//
// MC `net.minecraft.util.LightCoordsUtil` — the packed "light coords" every
// renderer passes around.
//
//   PACKED (flat):  block << 4 | sky << 20        (levels 0..15)
//   SMOOTH:         block8 | sky8 << 16            (0..240 each; smooth
//                                                   lighting's averages give
//                                                   quarter levels and the
//                                                   partial-face blend any
//                                                   value in between)
//
// The two share bit positions: a PACKED value read as SMOOTH is block*16 /
// sky*16, which is exactly the lightmap texture coordinate (uv / 256 picks the
// texel, see shaders' sampleLightmap). MC relies on that and so does this.
#pragma once

#include <algorithm>
#include <cstdint>

namespace Game::Lighting::LightCoords {

    inline constexpr int kFullBright = 15728880;   // pack(15, 15)
    inline constexpr int kFullSky    = 15728640;   // pack(0, 15)
    inline constexpr int kMaxSmoothLevel = 240;

    inline constexpr int Pack(int block, int sky) { return (block << 4) | (sky << 20); }
    inline constexpr int Block(int packed) { return (packed >> 4) & 15; }
    inline constexpr int Sky(int packed) { return (packed >> 20) & 15; }
    inline constexpr int WithBlock(int coords, int block) { return (coords & 16711680) | (block << 4); }

    inline constexpr int SmoothPack(int block, int sky) { return (block & 255) | ((sky & 255) << 16); }
    inline constexpr int SmoothBlock(int packed) { return packed & 255; }
    inline constexpr int SmoothSky(int packed) { return (packed >> 16) & 255; }

    inline constexpr int Max(int a, int b) {
        return Pack(std::max(Block(a), Block(b)), std::max(Sky(a), Sky(b)));
    }

    // MC LightCoordsUtil.smoothBlend (26.x): a neighbour that reads 0 (an
    // opaque block's own cell) takes the centre's value, one with no sky
    // takes the centre's sky — but only when the centre itself is lit above
    // 2, so a pitch-dark face is not brightened by its lit neighbours.
    inline constexpr int SmoothBlend(int n1, int n2, int n3, int center) {
        if (Sky(center) > 2 || Block(center) > 2) {
            if (n1 == 0) n1 = center; else if (Sky(n1) == 0) n1 |= center & 16711680;
            if (n2 == 0) n2 = center; else if (Sky(n2) == 0) n2 |= center & 16711680;
            if (n3 == 0) n3 = center; else if (Sky(n3) == 0) n3 |= center & 16711680;
        }
        return ((n1 + n2 + n3 + center) >> 2) & 16711935;
    }

    // MC LightCoordsUtil.smoothWeightedBlend — per-vertex bilinear weights of
    // the four corner values, for a quad that does not fill its cell's face.
    inline int SmoothWeightedBlend(int c1, int c2, int c3, int c4,
                                   float w1, float w2, float w3, float w4) {
        const int sky = static_cast<int>(static_cast<float>(SmoothSky(c1)) * w1 +
                                         static_cast<float>(SmoothSky(c2)) * w2 +
                                         static_cast<float>(SmoothSky(c3)) * w3 +
                                         static_cast<float>(SmoothSky(c4)) * w4);
        const int block = static_cast<int>(static_cast<float>(SmoothBlock(c1)) * w1 +
                                           static_cast<float>(SmoothBlock(c2)) * w2 +
                                           static_cast<float>(SmoothBlock(c3)) * w3 +
                                           static_cast<float>(SmoothBlock(c4)) * w4);
        return SmoothPack(block, sky);
    }

    // ── Coloured light tint (engine; ChunkLight's header note) ───────────
    // Packed coords leave bits 8..15 and 24..31 unused. The entity side
    // (EntityEnvironment::PackedLightAt / LevelLightCoordsAt) carries the
    // block light's tint there — each channel's shortfall from the brightest,
    // 5 bits, red in bits 8..12, green in 13..15 + 24..25, blue in 26..30 (bit
    // 31 stays clear, so coords stay non-negative). Every helper above masks
    // or rebuilds and so drops it (white): only a straight hand-off to
    // EntityEnvironment::LightColor keeps the colour. 0 = white.
    inline constexpr int kTintBitsMask = 0x7F00FF00;
    inline constexpr int Tint15(int coords) {
        return ((coords >> 8) & 0xFF) | (((coords >> 24) & 0x7F) << 8);
    }
    // `coords` with the tint of channel levels r, g, b (any common scale).
    inline constexpr int WithTint(int coords, int r, int g, int b) {
        const int top = std::max(r, std::max(g, b));
        coords &= ~kTintBitsMask;
        if (top <= 0 || (r == g && g == b)) return coords;
        const int sr = (31 * (top - r) + top / 2) / top;
        const int sg = (31 * (top - g) + top / 2) / top;
        const int sb = (31 * (top - b) + top / 2) / top;
        const int t = sr | (sg << 5) | (sb << 10);
        return coords | ((t & 0xFF) << 8) | (((t >> 8) & 0x7F) << 24);
    }

    // The 16-bit form the terrain vertex carries (TerrainVertex::light):
    // block8 in the low byte, sky8 in the high byte. Takes either format.
    inline constexpr uint16_t ToVertex(int smoothOrPacked) {
        return static_cast<uint16_t>((SmoothBlock(smoothOrPacked) & 255) |
                                     ((SmoothSky(smoothOrPacked) & 255) << 8));
    }

} // namespace Game::Lighting::LightCoords
