// File: src/common/world/block/BlockRegistry.cpp
#include "BlockRegistry.hpp"
#include "../../core/Log.hpp"

namespace Game {

    // Define the static array
    std::array<Block, BlockRegistry::Size> BlockRegistry::blockDefinitions{};

    void BlockRegistry::RegisterModelBlock(BlockID id, const std::string& name, RenderLayer layer,
                                              const std::string& modelName) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterModelBlock", static_cast<unsigned>(id));
            return;
        }

        bool opaque = (layer == RenderLayer::Opaque);
        blockDefinitions[index] = Block{
            .name = name,
            .opaque = opaque,
            .modelName = modelName,
            .legacyTexIdx = {0, 0, 0, 0, 0, 0},
            .useLegacyTextures = false,
            .isTransparent = !opaque,
            .renderLayer = layer
        };

        Log::Info("Registered model-based block ID %u as \"%s\" (model=%s, opaque=%s)",
                  static_cast<unsigned>(id), name.c_str(), modelName.c_str(),
                  opaque ? "true" : "false");
    }

    void BlockRegistry::RegisterLegacyBlock(BlockID id, const std::string& name, bool opaque,
                                                   const std::array<uint16_t, 6>& texIndices) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterLegacyBlock", static_cast<unsigned>(id));
            return;
        }

        blockDefinitions[index] = Block{
            .name = name,
            .opaque = opaque,
            .modelName = "",
            .legacyTexIdx = texIndices,
            .useLegacyTextures = true,
            .enableBiomeTinting = false,
            .isTransparent = !opaque,
            .renderLayer = opaque ? RenderLayer::Opaque : RenderLayer::Cutout
        };

        Log::Info("Registered legacy block ID %u as \"%s\" (legacy_textures=%s)",
                  static_cast<unsigned>(id), name.c_str(), opaque ? "opaque" : "transparent");
    }

    void BlockRegistry::Init() {
        Log::Info("Initializing Block Registry...");

        // SPECIAL: Air block - always transparent, no model
        RegisterLegacyBlock(BlockID::Air, "Air", false, {1008, 1008, 1008, 1008, 1008, 1008});

        // All blocks from BlockDefs.inc (single source of truth)
        #define BLOCK_DEF(e, m, d, r) RegisterModelBlock(BlockID::e, d, Game::RenderLayer::r, m);
        #include "BlockDefs.inc"
        #undef BLOCK_DEF


        // Manual entries not in all_blocks.txt
        RegisterModelBlock(BlockID::SnowGrass, "Snow Grass", RenderLayer::Opaque, "grass_block_snow");

        // Override model names for blocks where minecraft ID != model file name
        RegisterModelBlock(BlockID::Water, "Water", RenderLayer::Translucent, "water_still");
        RegisterModelBlock(BlockID::Lava, "Lava", RenderLayer::Translucent, "lava_still");

        // ── Infested-block model aliases. ───────────────────────────────────
        // MC's BlockModelGenerators.createInfestedStone (line 1726-1731) and
        // .createInfestedDeepslate (line 1734-1739) explicitly REUSE the
        // STONE / DEEPSLATE model — silverfish-bearing variants are visually
        // identical to the host block, only behaviour differs. Same pattern
        // for the brick / mossy / cracked / chiseled variants
        // (BlockModelGenerators.java:2490 — copyModel(STONE_BRICKS,
        // INFESTED_STONE_BRICKS), etc).
        RegisterModelBlock(BlockID::InfestedStone,              "Infested Stone",
                           RenderLayer::Opaque, "stone");
        RegisterModelBlock(BlockID::InfestedDeepslate,          "Infested Deepslate",
                           RenderLayer::Opaque, "deepslate");
        RegisterModelBlock(BlockID::InfestedCobblestone,        "Infested Cobblestone",
                           RenderLayer::Opaque, "cobblestone");
        RegisterModelBlock(BlockID::InfestedStoneBricks,        "Infested Stone Bricks",
                           RenderLayer::Opaque, "stone_bricks");
        RegisterModelBlock(BlockID::InfestedMossyStoneBricks,   "Infested Mossy Stone Bricks",
                           RenderLayer::Opaque, "mossy_stone_bricks");
        RegisterModelBlock(BlockID::InfestedCrackedStoneBricks, "Infested Cracked Stone Bricks",
                           RenderLayer::Opaque, "cracked_stone_bricks");
        RegisterModelBlock(BlockID::InfestedChiseledStoneBricks,"Infested Chiseled Stone Bricks",
                           RenderLayer::Opaque, "chiseled_stone_bricks");

        // ── Multi-state block model overrides. ──────────────────────────────
        // For each block whose bare name has no model JSON of its own (the
        // models live under `<name>_<state-suffix>` files), point the BARE
        // BlockID at its DEFAULT-STATE model (matching MC's
        // `Block.defaultBlockState()` convention). Variant BlockIDs added in
        // Blocks.hpp's manual section cover the non-default states; their
        // model registrations follow.

        // Double-tall plants — bare ID = lower half (DoubleBlockHalf.LOWER is MC's default).
        // Property: `half=upper|lower` (BlockStateProperties.java:177
        // DOUBLE_BLOCK_HALF). Lower-half model is `<name>_bottom`,
        // upper-half is `<name>_top`.
        RegisterModelBlock(BlockID::Lilac,        "Lilac (Lower)",
                           RenderLayer::Cutout, "lilac_bottom");
        RegisterModelBlock(BlockID::Peony,        "Peony (Lower)",
                           RenderLayer::Cutout, "peony_bottom");
        RegisterModelBlock(BlockID::RoseBush,     "Rose Bush (Lower)",
                           RenderLayer::Cutout, "rose_bush_bottom");
        RegisterModelBlock(BlockID::LargeFern,    "Large Fern (Lower)",
                           RenderLayer::Cutout, "large_fern_bottom");
        RegisterModelBlock(BlockID::TallSeagrass, "Tall Seagrass (Lower)",
                           RenderLayer::Cutout, "tall_seagrass_bottom");
        // Tall grass — biome-tinted double-plant (MC's TINTED PlantType,
        // BlockModelGenerators.java:613-617 createTintedDoublePlant which
        // applies the grass biome colour). Renders Cutout like other tall
        // plants. The biome-tint hook itself is already handled by the
        // mesher when `enableBiomeTinting` is set; double-plants in MC use
        // the same tint path as `short_grass`.
        RegisterModelBlock(BlockID::TallGrass,    "Tall Grass (Lower)",
                           RenderLayer::Cutout, "tall_grass_bottom");

        RegisterModelBlock(BlockID::LilacTop,        "Lilac (Upper)",
                           RenderLayer::Cutout, "lilac_top");
        RegisterModelBlock(BlockID::PeonyTop,        "Peony (Upper)",
                           RenderLayer::Cutout, "peony_top");
        RegisterModelBlock(BlockID::RoseBushTop,     "Rose Bush (Upper)",
                           RenderLayer::Cutout, "rose_bush_top");
        RegisterModelBlock(BlockID::LargeFernTop,    "Large Fern (Upper)",
                           RenderLayer::Cutout, "large_fern_top");
        RegisterModelBlock(BlockID::TallSeagrassTop, "Tall Seagrass (Upper)",
                           RenderLayer::Cutout, "tall_seagrass_top");
        RegisterModelBlock(BlockID::TallGrassTop,    "Tall Grass (Upper)",
                           RenderLayer::Cutout, "tall_grass_top");

        // Bee nest — bare ID = honey_level<5 (empty visual). MC's
        // BlockModelGenerators.createBeeNest (line 791-798) shows only
        // honey_level=5 dispatches to the honey model; 0..4 all use empty.
        // Property: BeehiveBlock.HONEY_LEVEL = LEVEL_HONEY = IntegerProperty
        // "honey_level", 0..5 (BlockStateProperties.java:200).
        RegisterModelBlock(BlockID::BeeNest,      "Bee Nest",
                           RenderLayer::Opaque, "bee_nest_empty");
        RegisterModelBlock(BlockID::BeeNestHoney, "Bee Nest (Honey)",
                           RenderLayer::Opaque, "bee_nest_honey");

        // Leaf litter — bare ID = segment_amount=1. Variants 2..4 are
        // separate BlockIDs because MC's IntegerProperty SEGMENT_AMOUNT
        // (BlockStateProperties.java:165: 1..4) drives a different model per
        // value (template_leaf_litter_1..4 with progressively more visible
        // segment quads).
        RegisterModelBlock(BlockID::LeafLitter,  "Leaf Litter",
                           RenderLayer::Cutout, "leaf_litter_1");
        RegisterModelBlock(BlockID::LeafLitter2, "Leaf Litter (2)",
                           RenderLayer::Cutout, "leaf_litter_2");
        RegisterModelBlock(BlockID::LeafLitter3, "Leaf Litter (3)",
                           RenderLayer::Cutout, "leaf_litter_3");
        RegisterModelBlock(BlockID::LeafLitter4, "Leaf Litter (4)",
                           RenderLayer::Cutout, "leaf_litter_4");

        // Wildflowers — same `segment_amount` pattern as leaf litter.
        RegisterModelBlock(BlockID::Wildflowers,  "Wildflowers",
                           RenderLayer::Cutout, "wildflowers_1");
        RegisterModelBlock(BlockID::Wildflowers2, "Wildflowers (2)",
                           RenderLayer::Cutout, "wildflowers_2");
        RegisterModelBlock(BlockID::Wildflowers3, "Wildflowers (3)",
                           RenderLayer::Cutout, "wildflowers_3");
        RegisterModelBlock(BlockID::Wildflowers4, "Wildflowers (4)",
                           RenderLayer::Cutout, "wildflowers_4");

        // Pink petals — same shape but DIFFERENT property name. MC uses
        // `flower_amount` (BlockStateProperties.java:164 FLOWER_AMOUNT)
        // rather than leaf_litter/wildflowers' `segment_amount` (line 165).
        // Bare ID = flower_amount=1; variants 2..4 each have their own
        // model file with progressively more visible petal quads.
        RegisterModelBlock(BlockID::PinkPetals,  "Pink Petals",
                           RenderLayer::Cutout, "pink_petals_1");
        RegisterModelBlock(BlockID::PinkPetals2, "Pink Petals (2)",
                           RenderLayer::Cutout, "pink_petals_2");
        RegisterModelBlock(BlockID::PinkPetals3, "Pink Petals (3)",
                           RenderLayer::Cutout, "pink_petals_3");
        RegisterModelBlock(BlockID::PinkPetals4, "Pink Petals (4)",
                           RenderLayer::Cutout, "pink_petals_4");

        Log::Info("Block Registry initialization complete - %zu blocks registered",
                 static_cast<size_t>(BlockID::Count));
    }

    const Block& BlockRegistry::Get(BlockID id) {
        size_t idx = static_cast<size_t>(id);
        if (idx >= blockDefinitions.size()) {
            Log::Error("BlockRegistry::Get() - invalid BlockID %u", static_cast<unsigned>(id));
            // Return air block as fallback
            return blockDefinitions[0];
        }
        return blockDefinitions[idx];
    }

    bool BlockRegistry::UsesModelRendering(BlockID id) {
        const Block& block = Get(id);
        return !block.useLegacyTextures;
    }

    const BlockModel& BlockRegistry::GetBlockModel(BlockID id) {
        const Block& block = Get(id);
        return BlockModelRegistry::GetModel(block.modelName);
    }

} // namespace Game
