#pragma once

#include <string>
#include <array>

namespace Game {

    // We’ll use a uint16_t under the hood to match our palette/palette-index style.
    enum class BlockID : uint16_t {
        Air   = 0,
        Stone = 1,
        Dirt  = 2,
        Grass = 3,
        // … expand later (Wood, Sand, Water, etc.)
        Count // Always keep this as the last entry.
    };

    struct Block {
        std::string       name;        // e.g. "Stone"
        bool              opaque;      // Does this block occlude light / render faces?
        std::array<uint8_t, 6> texIdx;  // Atlas index per face (0–N). { +X, -X, +Y, -Y, +Z, -Z }
    };

} // namespace Game
