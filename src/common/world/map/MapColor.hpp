// File: src/common/world/map/MapColor.hpp
//
// Port of net/minecraft/world/level/material/MapColor.java — the 62 map
// colours (ids 0..61, 0 = NONE) and the four brightness steps a map pixel is
// drawn at. A map pixel is one "packed id" byte: `colorId << 2 | brightness`.
//
// The per-block colour comes from the transcribed Blocks.java table
// (Block::mapColor, 0xRRGGBB, tools/gen_block_hardness.py). MC's colour
// values are all distinct, so the RGB maps back to its id exactly. The few
// blocks whose MapColor is a function of their state (a log's axis, a bed's
// part, wheat's age) are answered by GetBlockMapColor, which reads the state
// the way Blocks.java's lambdas do.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <cstdint>

namespace Game::MapColors {

    // MapColor ids (MapColor.java field order). Only the ones code names.
    enum : uint8_t {
        NONE = 0, GRASS = 1, SAND = 2, WOOL = 3, FIRE = 4, ICE = 5, METAL = 6, PLANT = 7,
        SNOW = 8, CLAY = 9, DIRT = 10, STONE = 11, WATER = 12, WOOD = 13, QUARTZ = 14,
        COLOR_ORANGE = 15, COLOR_MAGENTA = 16, COLOR_LIGHT_BLUE = 17, COLOR_YELLOW = 18,
        COLOR_LIGHT_GREEN = 19, COLOR_PINK = 20, COLOR_GRAY = 21, COLOR_LIGHT_GRAY = 22,
        COLOR_CYAN = 23, COLOR_PURPLE = 24, COLOR_BLUE = 25, COLOR_BROWN = 26,
        COLOR_GREEN = 27, COLOR_RED = 28, COLOR_BLACK = 29, GOLD = 30, DIAMOND = 31,
        LAPIS = 32, EMERALD = 33, PODZOL = 34, NETHER = 35,
        TERRACOTTA_WHITE = 36, TERRACOTTA_PINK = 42, TERRACOTTA_GRAY = 43,
        DEEPSLATE = 59, RAW_IRON = 60, GLOW_LICHEN = 61,
    };

    constexpr int kColorCount = 62;

    // MapColor.Brightness — id and the RGB scale it multiplies by.
    enum class Brightness : uint8_t { LOW = 0, NORMAL = 1, HIGH = 2, LOWEST = 3 };
    constexpr int BrightnessModifier(Brightness b) {
        switch (b) {
            case Brightness::LOW:    return 180;
            case Brightness::NORMAL: return 220;
            case Brightness::HIGH:   return 255;
            case Brightness::LOWEST: return 135;
        }
        return 220;
    }

    // MapColor.col for an id (0xRRGGBB); 0 for NONE / an unknown id.
    uint32_t ColorRgb(uint8_t colorId);

    // The id whose col is `rgb`, NONE when no MapColor has it.
    uint8_t IdFromRgb(uint32_t rgb);

    // MapColor.getPackedId(brightness).
    constexpr uint8_t PackedId(uint8_t colorId, Brightness b) {
        return static_cast<uint8_t>((colorId << 2) | (static_cast<uint8_t>(b) & 3));
    }

    // MapColor.getColorFromPackedId: the ARGB a map texel shows (0 — fully
    // transparent — for NONE, the paper background shows through).
    uint32_t ArgbFromPackedId(uint8_t packedId);

    // BlockState.getMapColor(level, pos) — the block's MapColor id.
    uint8_t GetBlockMapColor(BlockState state);

} // namespace Game::MapColors
