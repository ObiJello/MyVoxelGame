// File: src/engine/block/Blocks.hpp
#pragma once

#include <string>
#include <array>

namespace Game {

    // We'll use a uint16_t under the hood to match our palette/palette-index style.
    enum class BlockID : uint16_t {
        Air          = 0,
        Stone        = 1,
        Dirt         = 2,
        Grass        = 3,
        Sand         = 4,
        Sandstone    = 5,
        OakLog       = 6,
        Snow         = 7,
        SnowGrass    = 8,
        Ice          = 9,
        Glass        = 10,
        Bedrock      = 11,
        Water        = 12,
        Leaves       = 13,
        CherryLog    = 14,
        BirchLog     = 15,
        AcaciaLog    = 16,
        CherryLeaves = 17,
        CoalOre      = 18,
        RedstoneOre  = 19,
        LapisOre     = 20,
        IronOre      = 21,
        GoldOre      = 22,
        EmeraldOre   = 23,
        DiamondOre   = 24,
        Gravel       = 25,
        Mycelium     = 26,
        Deepslate    = 27,
        Lava         = 28,
        Diorite      = 29,
        Andesite     = 30,
        Granite      = 31,
        CopperOre    = 32,
        DeepslateLapisOre = 33,
        DeepslateIronOre = 34,
        DeepslateGoldOre = 35,
        DeepslateDiamondOre = 36,
        DeepslateCopperOre = 37,
        Tuff         = 38,
        ChiseledTuffBricks = 39,
        TuffBricks   = 40,
        WaxedCutCopper = 41,
        waxed_chiseled_copper = 42,
        Spawner      = 43,
        Cobblestone  = 44,
        MossyCobblestone = 45,
        polished_tuff = 46,
        waxed_oxidized_chiseled_copper = 47,
        Count // Always keep this as the last entry.
    };

} // namespace Game