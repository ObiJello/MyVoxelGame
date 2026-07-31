// File: src/common/world/block/BlockRegistry.cpp
#include "BlockRegistry.hpp"
#include "entity/BlockEntityTypes.hpp"
#include "../../core/Log.hpp"
#include <string_view>
#include <atomic>
#include <limits>
#include <array>

namespace Game {

    namespace {

        // ── Name-pattern classifier (MC parity heuristics) ─────────────────
        // Runs after every block is registered. Walks the registry, looks at
        // each block's model name + display name, and assigns destroyTime /
        // preferredTool / requiresCorrectTool / minTier based on MC's
        // Blocks.java categories. Specific overrides (obsidian, bedrock,
        // diamond_ore, etc.) follow in ApplyExplicitHardnessOverrides.
        struct MiningTraits {
            float      destroyTime;
            ToolType   preferredTool;
            bool       requiresCorrectTool;
            MiningTier minTier;
        };

        bool nameContains(std::string_view name, std::string_view needle) {
            return name.find(needle) != std::string_view::npos;
        }

        MiningTraits ClassifyByName(const std::string& modelName) {
            const std::string& n = modelName;

            // Instant-break: flowers, grasses, saplings, mushrooms, torches,
            // fire, signs, redstone wire/torch, sugar cane, kelp, seagrass,
            // hanging vines, leaf litter, wildflowers, pink petals, lilypad,
            // dead bush, fern, vine, scaffolding (0.0 destroyTime in MC).
            static constexpr std::string_view kInstantSubstr[] = {
                "_sapling", "_flower", "tulip", "allium", "azure_bluet",
                "oxeye_daisy", "cornflower", "lily_of_the_valley", "dandelion",
                "poppy", "wither_rose", "torchflower", "open_eyeblossom",
                "closed_eyeblossom", "pitcher_plant",
                "short_grass", "tall_grass", "fern", "large_fern",
                "dead_bush", "vine", "weeping_vines", "twisting_vines",
                "kelp", "seagrass", "sugar_cane", "lily_pad",
                "torch", "_carpet", "moss_carpet",
                "redstone_wire", "redstone_torch", "tripwire",
                "leaf_litter", "wildflowers", "pink_petals",
                "warped_roots", "crimson_roots", "warped_fungus", "crimson_fungus",
                "hanging_roots", "small_dripleaf", "spore_blossom",
                "glow_lichen", "sculk_vein", "bush", "firefly_bush",
                "azalea", "flowering_azalea", "_button",
            };
            for (auto sv : kInstantSubstr) if (nameContains(n, sv)) {
                return {0.0f, ToolType::None, false, MiningTier::Wood};
            }

            // Leaves: 0.2, prefers shears, but hoe also speeds them up.
            if (nameContains(n, "_leaves")) {
                return {0.2f, ToolType::Shears, false, MiningTier::Wood};
            }

            // Wool / cloth: shears preferred, 0.8.
            if (nameContains(n, "_wool") || n == "white_wool" || nameContains(n, "cobweb")) {
                if (nameContains(n, "cobweb")) {
                    return {4.0f, ToolType::Sword, true, MiningTier::Wood};
                }
                return {0.8f, ToolType::Shears, false, MiningTier::Wood};
            }

            // Sand / gravel / clay / dirt / podzol / mycelium / coarse_dirt /
            // mud / soul_sand / soul_soil / snow_block / snow: shovel, 0.5-0.6.
            static constexpr std::string_view kShovelSubstr[] = {
                "sand", "gravel", "_dirt", "dirt_path", "podzol", "mycelium",
                "mud", "soul_sand", "soul_soil", "coarse_dirt", "rooted_dirt",
                "snow_block", "snow", "clay",
            };
            for (auto sv : kShovelSubstr) if (nameContains(n, sv)) {
                float t = nameContains(n, "_block") ? 0.2f : 0.6f;
                if (n == "dirt" || nameContains(n, "_dirt") || n == "podzol" ||
                    n == "mycelium" || n == "coarse_dirt" || n == "rooted_dirt") t = 0.5f;
                if (n == "clay") t = 0.6f;
                if (n == "mud") t = 0.5f;
                if (nameContains(n, "soul_")) t = 0.5f;
                if (n == "snow") t = 0.1f;
                return {t, ToolType::Shovel, false, MiningTier::Wood};
            }

            // Grass block / dirt-like surface blocks
            if (n == "grass_block" || n == "grass_block_snow") {
                return {0.6f, ToolType::Shovel, false, MiningTier::Wood};
            }

            // Wood: logs, planks, fences, doors, slabs, stairs, signs.
            static constexpr std::string_view kWoodSubstr[] = {
                "_log", "_wood", "_planks", "_fence", "_door", "_trapdoor",
                "_slab", "_stairs", "_sign", "_shelf", "stripped_",
                "bookshelf", "crafting_table", "chest", "barrel", "loom",
                "smoker", "_pressure_plate", "ladder",
            };
            for (auto sv : kWoodSubstr) if (nameContains(n, sv)) {
                // Slabs/stairs of stone-like still need pickaxe — handled below
                // by the stone-substr check that runs first when matched.
                ToolType tool = ToolType::Axe;
                float t = 2.0f;
                if (nameContains(n, "_pressure_plate")) t = 0.5f;
                if (nameContains(n, "_door") || nameContains(n, "_trapdoor")) t = 3.0f;
                if (nameContains(n, "ladder")) { tool = ToolType::Axe; t = 0.4f; }
                if (nameContains(n, "bookshelf")) t = 1.5f;
                if (nameContains(n, "chest")) t = 2.5f;
                return {t, tool, false, MiningTier::Wood};
            }

            // Ores — gated by tier (covered explicitly below for the iconic
            // ones, but provide a default for any remaining `_ore`).
            if (nameContains(n, "_ore")) {
                MiningTier tier = MiningTier::Wood;
                if (nameContains(n, "iron_") || nameContains(n, "lapis_") ||
                    nameContains(n, "copper_")) tier = MiningTier::Stone;
                if (nameContains(n, "gold_") || nameContains(n, "redstone_") ||
                    nameContains(n, "diamond_") || nameContains(n, "emerald_")) {
                    tier = (nameContains(n, "diamond_") || nameContains(n, "emerald_") ||
                            nameContains(n, "gold_") || nameContains(n, "redstone_"))
                            ? MiningTier::Iron : MiningTier::Stone;
                }
                if (nameContains(n, "ancient_debris")) tier = MiningTier::Diamond;
                float t = 3.0f;
                if (nameContains(n, "deepslate_")) t = 4.5f;
                if (nameContains(n, "nether_quartz") || nameContains(n, "nether_gold")) t = 3.0f;
                return {t, ToolType::Pickaxe, true, tier};
            }

            // Stone family (stone/cobblestone/andesite/granite/diorite/basalt/
            // deepslate/bricks/etc.) — pickaxe required.
            static constexpr std::string_view kStoneSubstr[] = {
                "stone", "cobblestone", "andesite", "granite", "diorite",
                "basalt", "blackstone", "deepslate", "tuff", "calcite",
                "dripstone", "_bricks", "brick", "prismarine", "purpur",
                "end_stone", "netherrack", "magma_block", "obsidian",
                "anvil", "iron_block", "gold_block", "diamond_block",
                "emerald_block", "redstone_block", "lapis_block",
                "iron_bars", "iron_door", "iron_trapdoor",
                "smooth_", "polished_", "chiseled_", "cracked_", "mossy_",
                "_wall", "concrete", "terracotta", "glazed_terracotta",
                "quartz", "amethyst_block", "amethyst_cluster",
                "ice", "packed_ice", "blue_ice", "frosted_ice",
                "copper_block", "cut_copper", "weathered_copper",
                "raw_copper_block", "raw_iron_block", "raw_gold_block",
                "netherite_block", "honey_block", "honeycomb_block",
                "slime_block", "moss_block", "shroomlight",
                "mud_bricks", "packed_mud", "respawn_anchor",
            };
            bool isStone = false;
            for (auto sv : kStoneSubstr) if (nameContains(n, sv)) { isStone = true; break; }
            if (isStone) {
                float t = 1.5f;
                MiningTier tier = MiningTier::Wood;
                bool req = true;
                ToolType tool = ToolType::Pickaxe;

                if (nameContains(n, "cobblestone")) t = 2.0f;
                if (nameContains(n, "_bricks") || nameContains(n, "_brick")) t = 2.0f;
                if (nameContains(n, "deepslate")) t = 3.0f;
                if (nameContains(n, "blackstone")) t = 1.5f;
                if (nameContains(n, "basalt")) t = 1.25f;
                if (nameContains(n, "netherrack")) { t = 0.4f; req = false; }
                if (nameContains(n, "magma_block")) t = 0.5f;
                if (nameContains(n, "ice")) { t = 0.5f; req = false; tool = ToolType::Pickaxe; }
                if (nameContains(n, "packed_ice") || nameContains(n, "blue_ice")) t = 0.5f;

                // Metal blocks
                if (n == "iron_block") { t = 5.0f; tier = MiningTier::Stone; }
                if (n == "gold_block") { t = 3.0f; tier = MiningTier::Iron; }
                if (n == "diamond_block" || n == "emerald_block") { t = 5.0f; tier = MiningTier::Iron; }
                if (n == "netherite_block") { t = 50.0f; tier = MiningTier::Diamond; }
                if (n == "raw_iron_block") { t = 5.0f; tier = MiningTier::Stone; }
                if (n == "raw_gold_block") { t = 5.0f; tier = MiningTier::Iron; }
                if (n == "raw_copper_block" || nameContains(n, "copper_block") ||
                    nameContains(n, "cut_copper")) { t = 3.0f; tier = MiningTier::Stone; }
                if (n == "redstone_block" || n == "lapis_block") { t = 3.0f; tier = MiningTier::Stone; }
                if (n == "amethyst_block" || n == "amethyst_cluster") { t = 1.5f; tier = MiningTier::Wood; }

                if (n == "obsidian" || nameContains(n, "crying_obsidian")) {
                    t = 50.0f; tier = MiningTier::Diamond;
                }
                if (n == "respawn_anchor") { t = 50.0f; tier = MiningTier::Diamond; }
                if (n == "ancient_debris") { t = 30.0f; tier = MiningTier::Diamond; }

                // Honey/slime/moss/shroomlight — not stone, no tool needed.
                if (n == "honey_block" || n == "slime_block" ||
                    n == "moss_block" || n == "shroomlight" ||
                    n == "honeycomb_block") { return {0.5f, ToolType::None, false, MiningTier::Wood}; }

                // Terracotta / concrete / glazed
                if (nameContains(n, "concrete_powder")) {
                    return {0.5f, ToolType::Shovel, false, MiningTier::Wood};
                }
                if (nameContains(n, "concrete")) { t = 1.8f; tier = MiningTier::Wood; }
                if (nameContains(n, "terracotta")) { t = 1.25f; tier = MiningTier::Wood; }
                if (nameContains(n, "glazed_terracotta")) t = 1.4f;

                // Quartz family is breakable by any pickaxe
                if (nameContains(n, "quartz")) { t = 0.8f; tier = MiningTier::Wood; }

                // Walls / stairs / slabs of stone family — inherit tier from base
                return {t, tool, req, tier};
            }

            // Glass: 0.3, no tool, doesn't require a tool to break.
            if (nameContains(n, "glass") || nameContains(n, "_pane")) {
                return {0.3f, ToolType::None, false, MiningTier::Wood};
            }

            // Default: 1.0s, no special tool, no requirement.
            return {1.0f, ToolType::None, false, MiningTier::Wood};
        }

        void ApplyExplicitHardnessOverrides(std::array<Block, BlockRegistry::Size>& defs) {
            auto setHardness = [&](BlockID id, float dt, ToolType tool, bool req,
                                   MiningTier tier) {
                size_t idx = static_cast<size_t>(id);
                if (idx >= defs.size()) return;
                defs[idx].destroyTime = dt;
                defs[idx].preferredTool = tool;
                defs[idx].requiresCorrectTool = req;
                defs[idx].minTier = tier;
            };
            // The iconic / sanity-check set — values from MC Blocks.java.
            setHardness(BlockID::Air,          0.0f, ToolType::None,  false, MiningTier::Wood);
            setHardness(BlockID::Bedrock,     -1.0f, ToolType::None,  false, MiningTier::Wood);
            setHardness(BlockID::Stone,        1.5f, ToolType::Pickaxe, true,  MiningTier::Wood);
            setHardness(BlockID::Cobblestone,  2.0f, ToolType::Pickaxe, true,  MiningTier::Wood);
            setHardness(BlockID::Dirt,         0.5f, ToolType::Shovel,  false, MiningTier::Wood);
            setHardness(BlockID::Grass,        0.6f, ToolType::Shovel,  false, MiningTier::Wood);
            setHardness(BlockID::SnowGrass,    0.6f, ToolType::Shovel,  false, MiningTier::Wood);
            setHardness(BlockID::Sand,         0.5f, ToolType::Shovel,  false, MiningTier::Wood);
            setHardness(BlockID::Gravel,       0.6f, ToolType::Shovel,  false, MiningTier::Wood);
            setHardness(BlockID::Obsidian,    50.0f, ToolType::Pickaxe, true,  MiningTier::Diamond);
            setHardness(BlockID::CryingObsidian, 50.0f, ToolType::Pickaxe, true, MiningTier::Diamond);
            setHardness(BlockID::IronOre,      3.0f, ToolType::Pickaxe, true,  MiningTier::Stone);
            setHardness(BlockID::CoalOre,      3.0f, ToolType::Pickaxe, true,  MiningTier::Wood);
            setHardness(BlockID::CopperOre,    3.0f, ToolType::Pickaxe, true,  MiningTier::Stone);
            setHardness(BlockID::GoldOre,      3.0f, ToolType::Pickaxe, true,  MiningTier::Iron);
            setHardness(BlockID::DiamondOre,   3.0f, ToolType::Pickaxe, true,  MiningTier::Iron);
            setHardness(BlockID::EmeraldOre,   3.0f, ToolType::Pickaxe, true,  MiningTier::Iron);
            setHardness(BlockID::RedstoneOre,  3.0f, ToolType::Pickaxe, true,  MiningTier::Iron);
            setHardness(BlockID::LapisOre,     3.0f, ToolType::Pickaxe, true,  MiningTier::Stone);
            setHardness(BlockID::NetherQuartzOre, 3.0f, ToolType::Pickaxe, true, MiningTier::Wood);
            setHardness(BlockID::NetherGoldOre,   3.0f, ToolType::Pickaxe, true, MiningTier::Wood);
            setHardness(BlockID::AncientDebris,  30.0f, ToolType::Pickaxe, true, MiningTier::Diamond);
            setHardness(BlockID::Netherrack,   0.4f, ToolType::Pickaxe, true,  MiningTier::Wood);
            setHardness(BlockID::Glowstone,    0.3f, ToolType::None,    false, MiningTier::Wood);
            setHardness(BlockID::Sandstone,    0.8f, ToolType::Pickaxe, true,  MiningTier::Wood);
            setHardness(BlockID::Glass,        0.3f, ToolType::None,    false, MiningTier::Wood);
            setHardness(BlockID::IronBlock,    5.0f, ToolType::Pickaxe, true,  MiningTier::Stone);
            setHardness(BlockID::GoldBlock,    3.0f, ToolType::Pickaxe, true,  MiningTier::Iron);
            setHardness(BlockID::DiamondBlock, 5.0f, ToolType::Pickaxe, true,  MiningTier::Iron);
            setHardness(BlockID::EmeraldBlock, 5.0f, ToolType::Pickaxe, true,  MiningTier::Iron);
            setHardness(BlockID::NetheriteBlock, 50.0f, ToolType::Pickaxe, true, MiningTier::Diamond);
            setHardness(BlockID::Water,       -1.0f, ToolType::None,    false, MiningTier::Wood);
            setHardness(BlockID::Lava,        -1.0f, ToolType::None,    false, MiningTier::Wood);
        }

        // Slab pair table (bottom BlockID → top BlockID + model + display name).
        // Used both at Init time to register the top variants and at runtime by
        // SlabTopVariant / SlabBottomVariant / IsSlabTop. The runtime lookup
        // tables (below) are built once from this on first call.
        struct SlabPair { BlockID bottom; BlockID top; const char* model; const char* name; };
        constexpr SlabPair kSlabPairs[] = {
            { BlockID::AcaciaSlab,                     BlockID::AcaciaSlabTop,                     "acacia_slab_top",                     "Acacia Slab (Top)" },
            { BlockID::AndesiteSlab,                   BlockID::AndesiteSlabTop,                   "andesite_slab_top",                   "Andesite Slab (Top)" },
            { BlockID::BambooMosaicSlab,               BlockID::BambooMosaicSlabTop,               "bamboo_mosaic_slab_top",               "Bamboo Mosaic Slab (Top)" },
            { BlockID::BambooSlab,                     BlockID::BambooSlabTop,                     "bamboo_slab_top",                     "Bamboo Slab (Top)" },
            { BlockID::BirchSlab,                      BlockID::BirchSlabTop,                      "birch_slab_top",                      "Birch Slab (Top)" },
            { BlockID::BlackstoneSlab,                 BlockID::BlackstoneSlabTop,                 "blackstone_slab_top",                 "Blackstone Slab (Top)" },
            { BlockID::BrickSlab,                      BlockID::BrickSlabTop,                      "brick_slab_top",                      "Brick Slab (Top)" },
            { BlockID::CherrySlab,                     BlockID::CherrySlabTop,                     "cherry_slab_top",                     "Cherry Slab (Top)" },
            { BlockID::CobbledDeepslateSlab,           BlockID::CobbledDeepslateSlabTop,           "cobbled_deepslate_slab_top",           "Cobbled Deepslate Slab (Top)" },
            { BlockID::CobblestoneSlab,                BlockID::CobblestoneSlabTop,                "cobblestone_slab_top",                "Cobblestone Slab (Top)" },
            { BlockID::CrimsonSlab,                    BlockID::CrimsonSlabTop,                    "crimson_slab_top",                    "Crimson Slab (Top)" },
            { BlockID::CutCopperSlab,                  BlockID::CutCopperSlabTop,                  "cut_copper_slab_top",                  "Cut Copper Slab (Top)" },
            { BlockID::CutRedSandstoneSlab,            BlockID::CutRedSandstoneSlabTop,            "cut_red_sandstone_slab_top",            "Cut Red Sandstone Slab (Top)" },
            { BlockID::CutSandstoneSlab,               BlockID::CutSandstoneSlabTop,               "cut_sandstone_slab_top",               "Cut Sandstone Slab (Top)" },
            { BlockID::DarkOakSlab,                    BlockID::DarkOakSlabTop,                    "dark_oak_slab_top",                    "Dark Oak Slab (Top)" },
            { BlockID::DarkPrismarineSlab,             BlockID::DarkPrismarineSlabTop,             "dark_prismarine_slab_top",             "Dark Prismarine Slab (Top)" },
            { BlockID::DeepslateBrickSlab,             BlockID::DeepslateBrickSlabTop,             "deepslate_brick_slab_top",             "Deepslate Brick Slab (Top)" },
            { BlockID::DeepslateTileSlab,              BlockID::DeepslateTileSlabTop,              "deepslate_tile_slab_top",              "Deepslate Tile Slab (Top)" },
            { BlockID::DioriteSlab,                    BlockID::DioriteSlabTop,                    "diorite_slab_top",                    "Diorite Slab (Top)" },
            { BlockID::EndStoneBrickSlab,              BlockID::EndStoneBrickSlabTop,              "end_stone_brick_slab_top",              "End Stone Brick Slab (Top)" },
            { BlockID::ExposedCutCopperSlab,           BlockID::ExposedCutCopperSlabTop,           "exposed_cut_copper_slab_top",           "Exposed Cut Copper Slab (Top)" },
            { BlockID::GraniteSlab,                    BlockID::GraniteSlabTop,                    "granite_slab_top",                    "Granite Slab (Top)" },
            { BlockID::JungleSlab,                     BlockID::JungleSlabTop,                     "jungle_slab_top",                     "Jungle Slab (Top)" },
            { BlockID::MangroveSlab,                   BlockID::MangroveSlabTop,                   "mangrove_slab_top",                   "Mangrove Slab (Top)" },
            { BlockID::MossyCobblestoneSlab,           BlockID::MossyCobblestoneSlabTop,           "mossy_cobblestone_slab_top",           "Mossy Cobblestone Slab (Top)" },
            { BlockID::MossyStoneBrickSlab,            BlockID::MossyStoneBrickSlabTop,            "mossy_stone_brick_slab_top",            "Mossy Stone Brick Slab (Top)" },
            { BlockID::MudBrickSlab,                   BlockID::MudBrickSlabTop,                   "mud_brick_slab_top",                   "Mud Brick Slab (Top)" },
            { BlockID::NetherBrickSlab,                BlockID::NetherBrickSlabTop,                "nether_brick_slab_top",                "Nether Brick Slab (Top)" },
            { BlockID::OakSlab,                        BlockID::OakSlabTop,                        "oak_slab_top",                        "Oak Slab (Top)" },
            { BlockID::OxidizedCutCopperSlab,          BlockID::OxidizedCutCopperSlabTop,          "oxidized_cut_copper_slab_top",          "Oxidized Cut Copper Slab (Top)" },
            { BlockID::PaleOakSlab,                    BlockID::PaleOakSlabTop,                    "pale_oak_slab_top",                    "Pale Oak Slab (Top)" },
            { BlockID::PetrifiedOakSlab,               BlockID::PetrifiedOakSlabTop,               "petrified_oak_slab_top",               "Petrified Oak Slab (Top)" },
            { BlockID::PolishedAndesiteSlab,           BlockID::PolishedAndesiteSlabTop,           "polished_andesite_slab_top",           "Polished Andesite Slab (Top)" },
            { BlockID::PolishedBlackstoneBrickSlab,    BlockID::PolishedBlackstoneBrickSlabTop,    "polished_blackstone_brick_slab_top",    "Polished Blackstone Brick Slab (Top)" },
            { BlockID::PolishedBlackstoneSlab,         BlockID::PolishedBlackstoneSlabTop,         "polished_blackstone_slab_top",         "Polished Blackstone Slab (Top)" },
            { BlockID::PolishedDeepslateSlab,          BlockID::PolishedDeepslateSlabTop,          "polished_deepslate_slab_top",          "Polished Deepslate Slab (Top)" },
            { BlockID::PolishedDioriteSlab,            BlockID::PolishedDioriteSlabTop,            "polished_diorite_slab_top",            "Polished Diorite Slab (Top)" },
            { BlockID::PolishedGraniteSlab,            BlockID::PolishedGraniteSlabTop,            "polished_granite_slab_top",            "Polished Granite Slab (Top)" },
            { BlockID::PolishedTuffSlab,               BlockID::PolishedTuffSlabTop,               "polished_tuff_slab_top",               "Polished Tuff Slab (Top)" },
            { BlockID::PrismarineBrickSlab,            BlockID::PrismarineBrickSlabTop,            "prismarine_brick_slab_top",            "Prismarine Brick Slab (Top)" },
            { BlockID::PrismarineSlab,                 BlockID::PrismarineSlabTop,                 "prismarine_slab_top",                 "Prismarine Slab (Top)" },
            { BlockID::PurpurSlab,                     BlockID::PurpurSlabTop,                     "purpur_slab_top",                     "Purpur Slab (Top)" },
            { BlockID::QuartzSlab,                     BlockID::QuartzSlabTop,                     "quartz_slab_top",                     "Quartz Slab (Top)" },
            { BlockID::RedNetherBrickSlab,             BlockID::RedNetherBrickSlabTop,             "red_nether_brick_slab_top",             "Red Nether Brick Slab (Top)" },
            { BlockID::RedSandstoneSlab,               BlockID::RedSandstoneSlabTop,               "red_sandstone_slab_top",               "Red Sandstone Slab (Top)" },
            { BlockID::ResinBrickSlab,                 BlockID::ResinBrickSlabTop,                 "resin_brick_slab_top",                 "Resin Brick Slab (Top)" },
            { BlockID::SandstoneSlab,                  BlockID::SandstoneSlabTop,                  "sandstone_slab_top",                  "Sandstone Slab (Top)" },
            { BlockID::SmoothQuartzSlab,               BlockID::SmoothQuartzSlabTop,               "smooth_quartz_slab_top",               "Smooth Quartz Slab (Top)" },
            { BlockID::SmoothRedSandstoneSlab,         BlockID::SmoothRedSandstoneSlabTop,         "smooth_red_sandstone_slab_top",         "Smooth Red Sandstone Slab (Top)" },
            { BlockID::SmoothSandstoneSlab,            BlockID::SmoothSandstoneSlabTop,            "smooth_sandstone_slab_top",            "Smooth Sandstone Slab (Top)" },
            { BlockID::SmoothStoneSlab,                BlockID::SmoothStoneSlabTop,                "smooth_stone_slab_top",                "Smooth Stone Slab (Top)" },
            { BlockID::SpruceSlab,                     BlockID::SpruceSlabTop,                     "spruce_slab_top",                     "Spruce Slab (Top)" },
            { BlockID::StoneBrickSlab,                 BlockID::StoneBrickSlabTop,                 "stone_brick_slab_top",                 "Stone Brick Slab (Top)" },
            { BlockID::StoneSlab,                      BlockID::StoneSlabTop,                      "stone_slab_top",                      "Stone Slab (Top)" },
            { BlockID::TuffBrickSlab,                  BlockID::TuffBrickSlabTop,                  "tuff_brick_slab_top",                  "Tuff Brick Slab (Top)" },
            { BlockID::TuffSlab,                       BlockID::TuffSlabTop,                       "tuff_slab_top",                       "Tuff Slab (Top)" },
            { BlockID::WarpedSlab,                     BlockID::WarpedSlabTop,                     "warped_slab_top",                     "Warped Slab (Top)" },
            { BlockID::WaxedCutCopperSlab,             BlockID::WaxedCutCopperSlabTop,             "waxed_cut_copper_slab_top",             "Waxed Cut Copper Slab (Top)" },
            { BlockID::WaxedExposedCutCopperSlab,      BlockID::WaxedExposedCutCopperSlabTop,      "waxed_exposed_cut_copper_slab_top",      "Waxed Exposed Cut Copper Slab (Top)" },
            { BlockID::WaxedOxidizedCutCopperSlab,     BlockID::WaxedOxidizedCutCopperSlabTop,     "waxed_oxidized_cut_copper_slab_top",     "Waxed Oxidized Cut Copper Slab (Top)" },
            { BlockID::WaxedWeatheredCutCopperSlab,    BlockID::WaxedWeatheredCutCopperSlabTop,    "waxed_weathered_cut_copper_slab_top",    "Waxed Weathered Cut Copper Slab (Top)" },
            { BlockID::WeatheredCutCopperSlab,         BlockID::WeatheredCutCopperSlabTop,         "weathered_cut_copper_slab_top",         "Weathered Cut Copper Slab (Top)" },
        };

    } // anonymous namespace


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

        // ── Slab top-half variants — table lives at file scope above so the
        //    SlabTopVariant / SlabBottomVariant helpers can share it.
        for (const auto& p : kSlabPairs) {
            RegisterModelBlock(p.top, p.name, RenderLayer::Opaque, p.model);
        }

        // ── Mining data: classify every registered block by name, then
        //    apply explicit overrides for the iconic / sanity-check ones.
        //    Mirrors MC Blocks.java per-block strength() / requires-tool
        //    declarations. See ClassifyByName / ApplyExplicitHardnessOverrides
        //    above for the rules.
        //
        // ── Collision: blocks declared with `.noCollision()` in MC's
        //    Blocks.java (flowers, grasses, leaf litter, torches, redstone
        //    wire, vines, fungi, …) get hasCollision=false so the player
        //    walks straight through them. Match by name substring against the
        //    same vegetation/decoration list the instant-break classifier
        //    uses, plus a few extras (rails, signs, banner, end_rod, lever,
        //    string-style blocks). Carpets, buttons, slabs, fences, stairs,
        //    azaleas, etc. KEEP collision — they have non-empty MC shapes.
        static constexpr std::string_view kNoCollisionSubstr[] = {
            "_sapling", "_flower", "tulip", "allium", "azure_bluet",
            "oxeye_daisy", "cornflower", "lily_of_the_valley", "dandelion",
            "poppy", "wither_rose", "torchflower", "open_eyeblossom",
            "closed_eyeblossom", "pitcher_plant",
            "short_grass", "tall_grass", "fern", "large_fern",
            "dead_bush", "vine", "weeping_vines", "twisting_vines",
            "kelp", "seagrass", "sugar_cane",
            "torch",
            "redstone_wire", "redstone_torch", "tripwire",
            "leaf_litter", "wildflowers", "pink_petals",
            "warped_roots", "crimson_roots", "warped_fungus", "crimson_fungus",
            "hanging_roots", "spore_blossom",
            "glow_lichen", "sculk_vein", "firefly_bush",
            "_rail", "powered_rail", "detector_rail", "activator_rail",
            "_banner", "_sign", "_hanging_sign", "end_rod", "lever",
            "tripwire_hook", "string",
        };
        auto noCollisionFor = [](const std::string& n) -> bool {
            for (auto sv : kNoCollisionSubstr) {
                if (n.find(sv) != std::string::npos) return true;
            }
            return false;
        };
        for (size_t i = 0; i < blockDefinitions.size(); ++i) {
            Block& b = blockDefinitions[i];
            if (b.modelName.empty() && b.name.empty()) continue; // unregistered slot
            const std::string& name = !b.modelName.empty() ? b.modelName : b.name;
            const MiningTraits t = ClassifyByName(name);
            b.destroyTime         = t.destroyTime;
            b.preferredTool       = t.preferredTool;
            b.requiresCorrectTool = t.requiresCorrectTool;
            b.minTier             = t.minTier;
            b.hasCollision        = !noCollisionFor(name);
        }
        ApplyExplicitHardnessOverrides(blockDefinitions);

        Log::Info("Block Registry initialization complete - %zu blocks registered",
                 static_cast<size_t>(BlockID::Count));

        // BlockEntityTypes registers per-BlockID factories; must run AFTER
        // block ids are stable. Safe to call multiple times (idempotent).
        BlockEntityTypes::Initialize();
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

    bool BlockRegistry::HasCollision(BlockID id) {
        // SLAB TOP variants are ALWAYS collidable, no matter what the
        // noCollision classifier did during init. Mirrors the fast-path in
        // GetBlockShape — together these two guards rule out the entire
        // "top slab has no collision" failure mode regardless of how the
        // hasCollision flag was populated for their BlockID slot.
        if (IsSlabTop(id)) return true;
        return Get(id).hasCollision;
    }

    namespace {
        // Two BlockID-indexed lookup tables built once from kSlabPairs.
        // Cached behind a flag (std::call_once would also work but `static
        // bool` is fine for a single-threaded init since both PlayerSession
        // and the mesher only consult this AFTER BlockRegistry::Init has
        // run). BlockID::Air is the "not a slab" sentinel value.
        std::array<BlockID, BlockRegistry::Size>& SlabTopTable() {
            static std::array<BlockID, BlockRegistry::Size> t{};
            static bool built = false;
            if (!built) {
                t.fill(BlockID::Air);
                for (const auto& p : kSlabPairs) {
                    const auto bi = static_cast<size_t>(p.bottom);
                    if (bi < t.size()) t[bi] = p.top;
                }
                built = true;
            }
            return t;
        }
        std::array<BlockID, BlockRegistry::Size>& SlabBottomTable() {
            static std::array<BlockID, BlockRegistry::Size> t{};
            static bool built = false;
            if (!built) {
                t.fill(BlockID::Air);
                for (const auto& p : kSlabPairs) {
                    const auto ti = static_cast<size_t>(p.top);
                    if (ti < t.size()) t[ti] = p.bottom;
                }
                built = true;
            }
            return t;
        }
    }

    BlockID BlockRegistry::SlabTopVariant(BlockID bottom) {
        const auto i = static_cast<size_t>(bottom);
        if (i >= Size) return BlockID::Air;
        return SlabTopTable()[i];
    }

    BlockID BlockRegistry::SlabBottomVariant(BlockID top) {
        const auto i = static_cast<size_t>(top);
        if (i >= Size) return BlockID::Air;
        return SlabBottomTable()[i];
    }

    bool BlockRegistry::IsSlabTop(BlockID id) {
        return SlabBottomVariant(id) != BlockID::Air;
    }

    const BlockRegistry::BlockShape& BlockRegistry::GetBlockShape(BlockID id) {
        // Lazy per-BlockID cache. Computed on first request from the model's
        // elements; subsequent calls hit the cache. Sized for the full
        // BlockID enum so lookups are O(1) and lock-free after warm-up.
        static std::array<BlockShape, Size> s_cache{};
        static std::array<std::atomic<bool>, Size> s_computed{};

        const size_t idx = static_cast<size_t>(id);
        if (idx >= Size) {
            static const BlockShape kFull;
            return kFull;
        }

        // Fast-path: SLAB TOP variants ALWAYS resolve to y∈[0.5, 1] regardless
        // of whether the JSON model has loaded yet. Without this hardcode the
        // mesher's per-thread EnsureBlockPropsCache may populate s_cache with
        // the default full-cube shape for every SlabTop ID during the first
        // mesh build (before BlockModelRegistry::LoadModels has run, or
        // before the worker thread has imported its tables), permanently
        // mis-classifying them as full cubes — making the player fall right
        // through every top slab even though the model JSONs render correctly.
        // Bottom slabs work because their model name matches an existing
        // sentinel resolved at vanilla-init time.
        if (IsSlabTop(id)) {
            static const BlockShape kSlabTopShape =
                BlockShape{ glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(1.0f) };
            // Still memoise so subsequent lookups skip the IsSlabTop branch.
            s_cache[idx] = kSlabTopShape;
            s_computed[idx].store(true, std::memory_order_release);
            return s_cache[idx];
        }

        // Fast-path: BlockEntity-rendered blocks whose model JSON is empty
        // (BEWLR pattern) would otherwise fall through to the default full
        // cube. The BE renderer draws the chest/etc. at a smaller extent
        // than the cell — collision + outline + raycast should match the
        // actual visible shape, not a phantom 1×1×1.
        //
        // For chest variants the shape is `Block.column(14, 0, 14)` per
        // MC ChestBlock.java:320 → x,z ∈ [1/16, 15/16], y ∈ [0, 14/16].
        // Future BE renderers (shulker, bed, sign, …) can add their own
        // hardcoded shapes here as they ship.
        if (id == BlockID::Chest || id == BlockID::TrappedChest || id == BlockID::EnderChest) {
            static const BlockShape kChestShape =
                BlockShape{ glm::vec3(1.0f / 16.0f, 0.0f, 1.0f / 16.0f),
                            glm::vec3(15.0f / 16.0f, 14.0f / 16.0f, 15.0f / 16.0f) };
            s_cache[idx] = kChestShape;
            s_computed[idx].store(true, std::memory_order_release);
            return s_cache[idx];
        }

        if (s_computed[idx].load(std::memory_order_acquire)) {
            return s_cache[idx];
        }

        // Build shape by unioning every element's AABB. If the model is
        // empty (either truly empty, OR the model registry hasn't loaded
        // JSONs from disk yet because this is being called extremely early),
        // we DO NOT cache — leaving `s_computed[idx]` false so the next call
        // tries again once models are available. Caching a default full-cube
        // shape here would permanently mis-collide partial blocks (slabs,
        // leaf litter, …) whenever someone happens to query their shape
        // before model JSONs are loaded.
        BlockShape shape;
        const BlockModel& model = GetBlockModel(id);
        if (model.elements.empty()) {
            static const BlockShape kFull;
            return kFull;
        }
        {
            glm::vec3 mn(std::numeric_limits<float>::infinity());
            glm::vec3 mx(-std::numeric_limits<float>::infinity());
            for (const auto& e : model.elements) {
                mn = glm::min(mn, glm::min(e.from, e.to));
                mx = glm::max(mx, glm::max(e.from, e.to));
            }
            // Convert MC pixel-space [0,16] → block-space [0,1].
            mn *= (1.0f / 16.0f);
            mx *= (1.0f / 16.0f);
            // Expand degenerate axes (leaf litter's zero-thickness plane,
            // single-quad cross models) by a sub-pixel epsilon. Without
            // this the highlight wireframe shader divides by a zero-length
            // screen edge and produces NaN. The bump is well below visual
            // resolution but enough for the shader to compute an offset.
            constexpr float kEpsilon = 1.0f / 256.0f;
            for (int i = 0; i < 3; ++i) {
                if (mx[i] - mn[i] < kEpsilon) {
                    const float mid = 0.5f * (mn[i] + mx[i]);
                    mn[i] = mid - kEpsilon * 0.5f;
                    mx[i] = mid + kEpsilon * 0.5f;
                }
            }
            shape.min = glm::clamp(mn, glm::vec3(0.0f), glm::vec3(1.0f));
            shape.max = glm::clamp(mx, glm::vec3(0.0f), glm::vec3(1.0f));
        }

        s_cache[idx] = shape;
        s_computed[idx].store(true, std::memory_order_release);
        return s_cache[idx];
    }

} // namespace Game
