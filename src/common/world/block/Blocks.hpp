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

        // Manual entries not in all_blocks.txt — each represents a distinct
        // `BlockState` variant (a property combination) that MC dispatches to
        // a different model. We promote each visually-distinct variant to a
        // first-class BlockID rather than carrying property bytes per voxel.
        // This mirrors what MC does internally too: although blockstates are
        // authored as JSON dispatch trees, the runtime collapses every state
        // combination into a flat int (`BlockState.getId()`); we just bake
        // the unique variants into BlockIDs at compile time.
        //
        // Naming convention: <BlockBareName><PropertySuffix>. The BARE
        // BlockID kept from BlockDefs.inc represents the "default" state
        // (matching MC's `Block.defaultBlockState()`):
        //   BlockID::Lilac        = lilac{half=lower}
        //   BlockID::LilacTop     = lilac{half=upper}
        //   …same shape for Peony/RoseBush/LargeFern/TallSeagrass…
        //   BlockID::BeeNest      = bee_nest{honey_level=0..4}    (empty texture)
        //   BlockID::BeeNestHoney = bee_nest{honey_level=5}       (honey texture)
        //   BlockID::LeafLitter   = leaf_litter{segment_amount=1}
        //   BlockID::LeafLitter2..4 = …{segment_amount=2..4}
        //   BlockID::Wildflowers  = wildflowers{segment_amount=1}
        //   BlockID::Wildflowers2..4 = …{segment_amount=2..4}
        SnowGrass,            // grass_block{snowy:true}

        // Double-tall plants (`half=upper`). Lower-half models are reused
        // for the bare BlockID via a modelName override in BlockRegistry.
        LilacTop,             // lilac{half=upper}
        PeonyTop,             // peony{half=upper}
        RoseBushTop,          // rose_bush{half=upper}
        LargeFernTop,         // large_fern{half=upper}
        TallSeagrassTop,      // tall_seagrass{half=upper}
        TallGrassTop,         // tall_grass{half=upper}  — TINTED double-plant
                              // (BlockModelGenerators.java:2392 createTintedDoublePlant)

        // Bee nest with honey present (only honey_level=5 is visually distinct).
        BeeNestHoney,

        // Leaf litter — segments 2..4 (segment 1 is the bare BlockID).
        LeafLitter2,
        LeafLitter3,
        LeafLitter4,

        // Wildflowers — segments 2..4 (segment 1 is the bare BlockID).
        Wildflowers2,
        Wildflowers3,
        Wildflowers4,

        // Pink petals — flower_amount=2..4 (flower_amount=1 is the bare BlockID).
        // Property `flower_amount` (BlockStateProperties.java:164,
        // FLOWER_AMOUNT = IntegerProperty.create("flower_amount", 1, 4)).
        // Note: separate property name from leaf_litter/wildflowers'
        // `segment_amount` (line 165) — same shape, different name.
        PinkPetals2,
        PinkPetals3,
        PinkPetals4,

        Count // Always keep this as the last entry.
    };

} // namespace Game
