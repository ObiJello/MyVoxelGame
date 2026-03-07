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

        // Manual entries not in all_blocks.txt
        SnowGrass, // grass_block{snowy:true} custom variant

        Count // Always keep this as the last entry.
    };

} // namespace Game
