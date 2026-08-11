// File: src/common/world/block/BlockRegistry.cpp
#include "BlockRegistry.hpp"
#include "BlockStateModels.hpp"
#include "entity/BlockEntityTypes.hpp"
#include "../../core/Log.hpp"
#include <string_view>
#include <atomic>
#include <limits>
#include <array>
#include <memory>
#include <vector>

namespace Game {

    // Defined in BlockBehaviors.cpp — wires the per-block right-click callbacks
    // once the table exists. Mirrors ItemRegistry_RegisterBehaviors.
    void BlockRegistry_RegisterBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

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

            // Blocks.java:1509-1510 — both are .instabreak(). Exact names, for
            // the same reason as the no-collision table: fire_coral_block and
            // campfire are ordinary blocks that must keep their hardness.
            if (n == "fire" || n == "soul_fire") {
                return {0.0f, ToolType::None, false, MiningTier::Wood};
            }

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
            // Exact-match cases that must NOT go in the substring table above:
            // "fire" as a substring would also catch fire_coral_block (a solid
            // cube) and campfire, both of which keep their collision.
            // Blocks.java:1509-1510 — fire and soul_fire are .noCollision().
            if (n == "fire" || n == "soul_fire") return true;
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

        // Air and the fluids collide with nothing. MC's AirBlock and
        // LiquidBlock return Shapes.empty() from getCollisionShape
        // (LiquidBlock.java:69-73), so the player falls through all three.
        // The name-substring classifier above can't reach them — "Air",
        // "water" and "lava" match no vegetation/decoration pattern — and
        // Block::hasCollision defaults to true, so state it explicitly.
        // Physics relies on this: CheckCollision/HasSupportBelow consult
        // HasCollision alone, with no "is this block solid?" pre-filter.
        // minecraft:cave_air / void_air already alias to BlockID::Air in
        // SectionDataUnpacker, so these three IDs cover every case.
        for (BlockID id : { BlockID::Air, BlockID::Water, BlockID::Lava }) {
            blockDefinitions[static_cast<size_t>(id)].hasCollision = false;
        }

        ApplyExplicitHardnessOverrides(blockDefinitions);

        // Right-click behaviour (crafting table's menu, …). After registration
        // because it looks blocks up by model name.
        BlockRegistry_RegisterBehaviors(blockDefinitions);

        // Must run after every block is registered — it classifies from the
        // registered model names.
        InitBlockStates();

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

    const BlockModel& BlockRegistry::GetBlockModel(BlockID id, uint8_t stateIndex) {
        const std::string& stateModel = BlockStateModels::ModelNameFor(id, stateIndex);
        if (!stateModel.empty()) {
            return BlockModelRegistry::GetModel(stateModel);
        }
        return GetBlockModel(id);
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

    // ========================================================================
    // BLOCK STATES  (MC StateDefinition / createBlockStateDefinition)
    // ========================================================================

    uint8_t BlockRegistry::BlockStateDefinition::IndexOf(const PropertyMap& props) const {
        uint32_t index = 0;
        for (const auto& prop : properties) {
            index *= static_cast<uint32_t>(prop.values.size());
            auto it = props.find(prop.name);
            if (it == props.end()) continue;   // absent => default (value index 0)
            for (size_t v = 1; v < prop.values.size(); ++v) {
                if (prop.values[v] == it->second) { index += static_cast<uint32_t>(v); break; }
            }
        }
        return static_cast<uint8_t>(index);
    }

    BlockRegistry::BlockStateDefinition::PropertyMap
    BlockRegistry::BlockStateDefinition::PropertiesOf(uint8_t stateIndex) const {
        PropertyMap out;
        uint32_t remaining = stateIndex;
        // Decode least-significant (last) property first, so walk backwards.
        for (size_t i = properties.size(); i-- > 0; ) {
            const auto& prop = properties[i];
            const uint32_t radix = static_cast<uint32_t>(prop.values.size());
            const uint32_t v = remaining % radix;
            remaining /= radix;
            out[prop.name] = prop.values[v];
        }
        return out;
    }

    std::string_view BlockRegistry::BlockStateDefinition::ValueOf(uint8_t stateIndex,
                                                                 std::string_view propName) const {
        uint32_t remaining = stateIndex;
        for (size_t i = properties.size(); i-- > 0; ) {
            const auto& prop = properties[i];
            const uint32_t radix = static_cast<uint32_t>(prop.values.size());
            const uint32_t v = remaining % radix;
            remaining /= radix;
            if (prop.name == propName) return prop.values[v];
        }
        return {};
    }

    uint8_t BlockRegistry::BlockStateDefinition::IndexOfSingle(std::string_view propName,
                                                              std::string_view value) const {
        uint32_t index = 0;
        for (const auto& prop : properties) {
            index *= static_cast<uint32_t>(prop.values.size());
            if (prop.name != propName) continue;   // other properties stay default
            for (size_t v = 1; v < prop.values.size(); ++v) {
                if (prop.values[v] == value) { index += static_cast<uint32_t>(v); break; }
            }
        }
        return static_cast<uint8_t>(index);
    }

    namespace {
        // One entry per BlockID; blocks with no properties keep an empty
        // definition (a single state, index 0). Built once in Init().
        std::array<BlockRegistry::BlockStateDefinition, BlockRegistry::Size> s_stateDefs{};

        // Property value lists. DEFAULT VALUE FIRST — see the invariant on
        // BlockStateDefinition. The defaults match MC's registerDefaultState
        // for each family (north for horizontal facing, north for 6-way facing,
        // y for pillar axis).
        const std::vector<std::string> kHorizontalFacingValues{"north", "east", "south", "west"};
        const std::vector<std::string> kFacingValues{"north", "east", "south", "west", "up", "down"};
        const std::vector<std::string> kAxisValues{"y", "x", "z"};
        // "false" first so state index 0 is the all-disconnected default, per
        // BlockStateDefinition's default-first invariant.
        const std::vector<std::string> kBoolValues{"false", "true"};

        bool NameHas(const std::string& n, std::string_view sub) {
            return n.find(sub) != std::string::npos;
        }
        bool NameIs(const std::string& n, std::string_view exact) {
            return n == exact;
        }

        // Which property set (if any) a block carries, decided from its MC
        // model name. Name-pattern matching rather than a hand-written BlockID
        // list, matching how ClassifyByName / kNoCollisionSubstr already work
        // in this file — new blocks from an MC version bump are picked up
        // automatically instead of silently defaulting to "no states".
        enum class StateKind { None, HorizontalFacing, Facing6, PillarAxis, FireConnections };

        StateKind ClassifyStates(const std::string& n) {
            // ── Fire (MC FireBlock) ─────────────────────────────────────────
            // All six entries of blockstates/fire.json carry a `when` asking
            // about north/east/south/west/up. BlockStateModels refuses a
            // multipart file outright unless it can answer every property the
            // file mentions, so without these five declared here fire resolves
            // to the default cube and renders as stone.
            //
            // Deliberately NOT `age`, even though FireBlock.java:264 declares
            // it: the blockstate never dispatches on age, and 16 values would
            // multiply this to 512 states — past what the uint8_t state index
            // in ChunkSection can hold.
            //
            // soul_fire is excluded on purpose. SoulFireBlock extends
            // BaseFireBlock and adds no properties, which is exactly why
            // blockstates/soul_fire.json has no `when` clauses at all and
            // already resolved fine.
            //
            // Exact name, not a substring: "fire" would also catch
            // fire_coral_block (a solid cube) and campfire.
            if (NameIs(n, "fire")) {
                return StateKind::FireConnections;
            }

            // ── Pillar axis (MC RotatedPillarBlock.getStateForPlacement) ────
            // Careful with ordering: "_wood" would also match "stripped_*_wood",
            // which is intended — every one of those is a RotatedPillarBlock.
            if (NameHas(n, "_log") || NameHas(n, "_wood") || NameHas(n, "_stem") ||
                NameHas(n, "_hyphae") || NameHas(n, "_pillar") ||
                NameIs(n, "bone_block") || NameIs(n, "hay_block") ||
                NameIs(n, "basalt") || NameIs(n, "polished_basalt") ||
                NameIs(n, "deepslate") || NameIs(n, "muddy_mangrove_roots") ||
                NameIs(n, "ochre_froglight") || NameIs(n, "verdant_froglight") ||
                NameIs(n, "pearlescent_froglight")) {
                return StateKind::PillarAxis;
            }

            // ── Six-way facing (MC DirectionalBlock) ────────────────────────
            if (NameIs(n, "dispenser") || NameIs(n, "dropper") || NameIs(n, "observer") ||
                NameIs(n, "barrel") || NameIs(n, "piston") || NameIs(n, "sticky_piston") ||
                NameIs(n, "command_block") || NameIs(n, "repeating_command_block") ||
                NameIs(n, "chain_command_block") || NameHas(n, "shulker_box") ||
                NameIs(n, "hopper") || NameIs(n, "lightning_rod") ||
                NameIs(n, "end_rod") || NameHas(n, "amethyst_bud") ||
                NameIs(n, "amethyst_cluster") || NameIs(n, "crafter")) {
                return StateKind::Facing6;
            }

            // ── Horizontal facing (MC HorizontalDirectionalBlock) ───────────
            if (NameHas(n, "_glazed_terracotta") ||
                NameIs(n, "furnace") || NameIs(n, "blast_furnace") || NameIs(n, "smoker") ||
                NameIs(n, "chest") || NameIs(n, "trapped_chest") || NameIs(n, "ender_chest") ||
                NameIs(n, "carved_pumpkin") || NameIs(n, "jack_o_lantern") ||
                NameIs(n, "loom") || NameIs(n, "stonecutter") || NameIs(n, "lectern") ||
                NameIs(n, "chiseled_bookshelf") || NameIs(n, "beehive") || NameIs(n, "bee_nest") ||
                NameIs(n, "end_portal_frame") || NameIs(n, "vault") ||
                NameIs(n, "anvil") || NameIs(n, "chipped_anvil") || NameIs(n, "damaged_anvil") ||
                NameIs(n, "grindstone") || NameIs(n, "campfire") || NameIs(n, "soul_campfire") ||
                NameIs(n, "decorated_pot") || NameIs(n, "calibrated_sculk_sensor") ||
                NameIs(n, "big_dripleaf") || NameIs(n, "small_dripleaf") ||
                NameIs(n, "ladder") || NameIs(n, "cocoa") ||
                NameIs(n, "repeater") || NameIs(n, "comparator") ||
                NameHas(n, "_stairs") ||
                // Segmented ground cover — MC LeafLitterBlock and
                // FlowerBedBlock (pink_petals, wildflowers), both
                // SegmentableBlock with HORIZONTAL_FACING defaulting to north
                // (LeafLitterBlock.java:25, FlowerBedBlock.java:35).
                //
                // Matched by substring because the segment count is baked into
                // the model name here (leaf_litter_1 … leaf_litter_4): this
                // engine spends a BlockID per `segment_amount` value, so only
                // `facing` is left to carry as state. Without it every clump in
                // the world sits in the same corner of its block pointing the
                // same way, which reads as a repeating grid rather than scatter.
                NameHas(n, "leaf_litter") || NameHas(n, "wildflowers") ||
                NameHas(n, "pink_petals")) {
                return StateKind::HorizontalFacing;
            }

            return StateKind::None;
        }
    } // namespace

    const BlockRegistry::BlockStateDefinition& BlockRegistry::GetStateDefinition(BlockID id) {
        const size_t idx = static_cast<size_t>(id);
        if (idx >= Size) {
            static const BlockStateDefinition kEmpty;
            return kEmpty;
        }
        return s_stateDefs[idx];
    }

    void BlockRegistry::InitBlockStates() {
        for (size_t i = 0; i < blockDefinitions.size(); ++i) {
            const Block& b = blockDefinitions[i];
            if (b.modelName.empty() && b.name.empty()) continue; // unregistered slot
            const std::string& name = !b.modelName.empty() ? b.modelName : b.name;

            BlockStateDefinition def;
            switch (ClassifyStates(name)) {
                case StateKind::HorizontalFacing:
                    def.properties.push_back({"facing", kHorizontalFacingValues});
                    break;
                case StateKind::Facing6:
                    def.properties.push_back({"facing", kFacingValues});
                    break;
                case StateKind::PillarAxis:
                    def.properties.push_back({"axis", kAxisValues});
                    break;
                case StateKind::FireConnections:
                    // 2^5 = 32 states; index 0 is all-false, which is the state
                    // MC itself uses whenever fire sits on a solid or burnable
                    // block (FireBlock.getStateForPlacement returns
                    // defaultBlockState() in that case) — i.e. every fire a
                    // flint and steel lights on the ground.
                    def.properties.push_back({"north", kBoolValues});
                    def.properties.push_back({"east",  kBoolValues});
                    def.properties.push_back({"south", kBoolValues});
                    def.properties.push_back({"west",  kBoolValues});
                    def.properties.push_back({"up",    kBoolValues});
                    break;
                case StateKind::None:
                    break;
            }
            s_stateDefs[i] = std::move(def);
        }

        size_t stateful = 0;
        for (const auto& d : s_stateDefs) if (!d.properties.empty()) ++stateful;
        Log::Info("Block states initialized - %zu of %zu blocks carry state properties",
                  stateful, static_cast<size_t>(BlockID::Count));
    }

    namespace {
        // Flat (BlockID, stateIndex) → shape cache.
        //
        // One slot per STATE, not per block: rotation lives in the model, so
        // `leaf_litter{facing=east}` and `leaf_litter{facing=north}` occupy
        // different quarters of their cell and need different shapes. Laid out
        // as a single flat array with a per-BlockID base offset so a lookup
        // stays one index and one relaxed atomic load — the mesher and the
        // physics sweep both call this per voxel from several threads, so a
        // map + mutex here would reintroduce exactly the contention that was
        // measured and removed from MyTerrainGenerator::MapBlockType.
        //
        // Sized from the state definitions, which are final once
        // BlockRegistry::Init has run. Total is roughly Size + a few hundred
        // (only ~1% of blocks carry properties), i.e. tens of KB.
        struct StateShapeCache {
            std::array<uint32_t, BlockRegistry::Size> base{};
            // State count captured AT BUILD TIME. Indices must be clamped
            // against this, never against a freshly-read StateCount(): the
            // table is sized once, so if anything queries a shape before
            // InitBlockStates has run every block is one state wide, and
            // clamping against the later, larger count would index straight
            // out of a block's slice and into the next one's.
            std::array<uint16_t, BlockRegistry::Size> count{};
            std::vector<BlockRegistry::BlockShape>    shapes;
            std::unique_ptr<std::atomic<bool>[]>      computed;
            uint32_t                                  total = 0;

            StateShapeCache() {
                uint32_t off = 0;
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    base[i] = off;
                    const uint16_t n =
                        BlockRegistry::GetStateDefinition(static_cast<BlockID>(i)).StateCount();
                    count[i] = (n > 0 ? n : 1);
                    off += count[i];
                }
                total = off;
                shapes.assign(off, BlockRegistry::BlockShape{});
                computed = std::make_unique<std::atomic<bool>[]>(off);
                for (uint32_t k = 0; k < off; ++k) {
                    computed[k].store(false, std::memory_order_relaxed);
                }
            }
        };

        // Magic static: thread-safe one-time init, and lazy enough that the
        // state definitions are already populated by the time anything asks
        // for a shape.
        StateShapeCache& StateShapes() {
            static StateShapeCache c;
            return c;
        }

        // Height in MC pixels for blocks whose collision/selection shape is
        // authored INDEPENDENTLY of their model, or 0 for "derive from model".
        //
        // MC's SegmentableBlock builds its shape as
        // `Block.box(0, 0, 0, 8, getShapeHeight(), 8)` rotated per facing
        // (SegmentableBlock.java) — anchored on the ground, with a real
        // thickness. The MODEL meanwhile is a zero-thickness plane hovering at
        // y=0.25px. Deriving the shape from that geometry gives a sliver about
        // 1/16 as tall as MC's box, floating just off the floor: it renders
        // fine but is close to impossible to put a crosshair on, which reads as
        // "the hitbox isn't where the block is".
        //
        // Only Y is overridden — X and Z still come from the model, so they
        // keep following the state's rotation and segment count.
        float AuthoredShapeHeightPx(const std::string& modelName) {
            auto has = [&](std::string_view s) {
                return modelName.find(s) != std::string::npos;
            };
            // LeafLitterBlock inherits SegmentableBlock's default 1.0.
            if (has("leaf_litter")) return 1.0f;
            // FlowerBedBlock (pink_petals, wildflowers) overrides to 3.0
            // (FlowerBedBlock.java:59).
            if (has("wildflowers") || has("pink_petals")) return 3.0f;
            return 0.0f;
        }
    } // namespace

    namespace {
        // MC Blocks.java `.offsetType(...)` declarations, extracted verbatim.
        // XYZ additionally sinks the block, which is what breaks up the flat
        // top line of a grass field.
        constexpr std::string_view kOffsetXYZ[] = {
            "fern", "short_grass", "short_dry_grass", "tall_dry_grass",
            "small_dripleaf",
        };
        constexpr std::string_view kOffsetXZ[] = {
            "allium", "azure_bluet", "bamboo", "bamboo_sapling", "blue_orchid",
            "closed_eyeblossom", "cornflower", "crimson_roots", "dandelion",
            "hanging_roots", "large_fern", "lilac", "lily_of_the_valley",
            "mangrove_propagule", "nether_sprouts", "open_eyeblossom",
            "orange_tulip", "oxeye_daisy", "peony", "pink_tulip",
            "pitcher_plant", "pointed_dripstone", "poppy", "red_tulip",
            "rose_bush", "sunflower", "tall_grass", "tall_seagrass",
            "torchflower", "warped_roots", "white_tulip", "wither_rose",
        };

        // MC Mth.getSeed(x, y, z), verbatim. The overflow is load-bearing —
        // this is a hash, and Java's wrapping arithmetic is the definition.
        int64_t MthGetSeed(int x, int y, int z) {
            uint64_t seed = static_cast<uint64_t>(static_cast<int64_t>(
                                static_cast<int32_t>(static_cast<uint32_t>(x) * 3129871u)))
                          ^ (static_cast<uint64_t>(static_cast<int64_t>(z)) * 116129781ull)
                          ^ static_cast<uint64_t>(static_cast<int64_t>(y));
            seed = seed * seed * 42317861ull + seed * 11ull;
            return static_cast<int64_t>(seed) >> 16;
        }
    } // namespace

    BlockRegistry::OffsetType BlockRegistry::GetOffsetType(BlockID id) {
        const size_t idx = static_cast<size_t>(id);
        if (idx >= Size) return OffsetType::None;

        // Cached per BlockID: this is asked once per rendered block face.
        static std::array<OffsetType, Size> s_table{};
        static bool s_built = false;
        if (!s_built) {
            for (size_t i = 0; i < Size; ++i) {
                const std::string& n = blockDefinitions[i].modelName;
                OffsetType t = OffsetType::None;
                for (std::string_view s : kOffsetXYZ) if (n == s) { t = OffsetType::XYZ; break; }
                if (t == OffsetType::None) {
                    for (std::string_view s : kOffsetXZ) if (n == s) { t = OffsetType::XZ; break; }
                }
                // Double plants are split across two BlockIDs here, with
                // _bottom / _top model names, so the exact matches above miss
                // the halves of tall_grass / large_fern.
                if (t == OffsetType::None &&
                    (n.rfind("tall_grass", 0) == 0 || n.rfind("large_fern", 0) == 0 ||
                     n.rfind("lilac", 0) == 0 || n.rfind("peony", 0) == 0 ||
                     n.rfind("rose_bush", 0) == 0 || n.rfind("sunflower", 0) == 0 ||
                     n.rfind("pitcher_plant", 0) == 0 || n.rfind("tall_seagrass", 0) == 0)) {
                    t = OffsetType::XZ;
                }
                s_table[i] = t;
            }
            s_built = true;
        }
        return s_table[idx];
    }

    glm::vec3 BlockRegistry::GetBlockOffset(BlockID id, int worldX, int worldZ) {
        const OffsetType type = GetOffsetType(id);
        if (type == OffsetType::None) return glm::vec3(0.0f);

        // MC BlockBehaviour.Properties.offsetType, cases XZ and XYZ:
        //   long seed = Mth.getSeed(x, 0, z);
        //   double dx = clamp(((seed & 15) / 15.0f - 0.5) * 0.5, -maxH, maxH);
        //   double dz = clamp(((seed >> 8 & 15) / 15.0f - 0.5) * 0.5, -maxH, maxH);
        //   double dy = ((seed >> 4 & 15) / 15.0f - 1.0) * maxV;   // XYZ only
        // Seeded with y = 0 on purpose, so both halves of a double plant land
        // on the same offset instead of shearing apart.
        constexpr float kMaxHorizontal = 0.25f;   // getMaxHorizontalOffset default
        constexpr float kMaxVertical   = 0.2f;    // getMaxVerticalOffset default

        const int64_t seed = MthGetSeed(worldX, 0, worldZ);

        const float fx = static_cast<float>(seed & 15LL) / 15.0f;
        const float fz = static_cast<float>((seed >> 8) & 15LL) / 15.0f;
        const float dx = std::clamp((fx - 0.5f) * 0.5f, -kMaxHorizontal, kMaxHorizontal);
        const float dz = std::clamp((fz - 0.5f) * 0.5f, -kMaxHorizontal, kMaxHorizontal);

        float dy = 0.0f;
        if (type == OffsetType::XYZ) {
            const float fy = static_cast<float>((seed >> 4) & 15LL) / 15.0f;
            dy = (fy - 1.0f) * kMaxVertical;   // always <= 0: the plant sinks
        }
        return glm::vec3(dx, dy, dz);
    }

    const BlockRegistry::BlockShape& BlockRegistry::GetBlockShape(BlockID id) {
        return GetBlockShape(id, 0);
    }

    const BlockRegistry::BlockShape& BlockRegistry::GetBlockShape(BlockID id, uint8_t stateIndex) {
        const size_t idx = static_cast<size_t>(id);
        if (idx >= Size) {
            static const BlockShape kFull;
            return kFull;
        }

        StateShapeCache& cache = StateShapes();
        // An out-of-range state index means a save or a peer described a state
        // this build doesn't model. Fall back to the default state rather than
        // indexing past the block's slice into the next block's shapes.
        if (stateIndex >= cache.count[idx]) stateIndex = 0;

        const uint32_t slot = cache.base[idx] + stateIndex;
        BlockShape* const shapes = cache.shapes.data();
        std::atomic<bool>* const computed = cache.computed.get();

        // Fast-path: SLAB TOP variants ALWAYS resolve to y∈[0.5, 1] regardless
        // of whether the JSON model has loaded yet. Without this hardcode the
        // mesher's per-thread EnsureBlockPropsCache may populate the cache with
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
            shapes[slot] = kSlabTopShape;
            computed[slot].store(true, std::memory_order_release);
            return shapes[slot];
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
        // Fire's model is a set of 22.4-pixel-tall flame quads that lean past
        // the top of the cell, so deriving the shape from the geometry would
        // hand the player a selection box 1.4 blocks high. MC keeps the shape
        // independent of the visual: BaseFireBlock.java:30 is
        // Block.column(16, 0, 1) — full footprint, one pixel tall.
        // Collision is already off, so this only governs the outline and the
        // raycast target.
        if (id == BlockID::Fire || id == BlockID::SoulFire) {
            static const BlockShape kFireShape =
                BlockShape{ glm::vec3(0.0f), glm::vec3(1.0f, 1.0f / 16.0f, 1.0f) };
            shapes[slot] = kFireShape;
            computed[slot].store(true, std::memory_order_release);
            return shapes[slot];
        }

        if (id == BlockID::Chest || id == BlockID::TrappedChest || id == BlockID::EnderChest) {
            static const BlockShape kChestShape =
                BlockShape{ glm::vec3(1.0f / 16.0f, 0.0f, 1.0f / 16.0f),
                            glm::vec3(15.0f / 16.0f, 14.0f / 16.0f, 15.0f / 16.0f) };
            shapes[slot] = kChestShape;
            computed[slot].store(true, std::memory_order_release);
            return shapes[slot];
        }

        if (computed[slot].load(std::memory_order_acquire)) {
            return shapes[slot];
        }

        // Build shape by unioning every element's AABB. If the model is
        // empty (either truly empty, OR the model registry hasn't loaded
        // JSONs from disk yet because this is being called extremely early),
        // we DO NOT cache — leaving `computed[slot]` false so the next call
        // tries again once models are available. Caching a default full-cube
        // shape here would permanently mis-collide partial blocks (slabs,
        // leaf litter, …) whenever someone happens to query their shape
        // before model JSONs are loaded.
        //
        // The STATE's model, not the block's: for a rotated state that model
        // is the synthesised `<model>__x0_yN`, whose elements are already in
        // their rotated positions, so the union below lands on the right
        // quarter of the cell without any extra rotation maths here.
        BlockShape shape;
        const BlockModel& model = GetBlockModel(id, stateIndex);
        if (model.elements.empty()) {
            static const BlockShape kFull;
            return kFull;
        }
        {
            glm::vec3 mn(std::numeric_limits<float>::infinity());
            glm::vec3 mx(-std::numeric_limits<float>::infinity());
            for (const auto& e : model.elements) {
                if (!e.rotation.IsIdentity()) {
                    // An element with its own rotation is authored at its
                    // PRE-rotation coordinates, which for fanned geometry sit
                    // well outside the cell (flowerbed stems reach x≈18).
                    // Unioning those raw bounds inflated the shape to the whole
                    // block. Rotate all eight corners the way the mesher rotates
                    // the vertices so the shape matches what is actually drawn.
                    for (int corner = 0; corner < 8; ++corner) {
                        const glm::vec3 p{
                            (corner & 1) ? e.to.x : e.from.x,
                            (corner & 2) ? e.to.y : e.from.y,
                            (corner & 4) ? e.to.z : e.from.z,
                        };
                        const glm::vec3 r = ApplyElementRotation(p, e.rotation);
                        mn = glm::min(mn, r);
                        mx = glm::max(mx, r);
                    }
                    continue;
                }
                mn = glm::min(mn, glm::min(e.from, e.to));
                mx = glm::max(mx, glm::max(e.from, e.to));
            }
            // Convert MC pixel-space [0,16] → block-space [0,1].
            mn *= (1.0f / 16.0f);
            mx *= (1.0f / 16.0f);

            // Blocks whose vertical extent MC authors separately from the
            // model get it back here, on the ground where MC puts it. X and Z
            // stay model-derived so rotation and segment count still drive the
            // footprint.
            if (const float hPx = AuthoredShapeHeightPx(blockDefinitions[idx].modelName);
                hPx > 0.0f) {
                mn.y = 0.0f;
                mx.y = hPx / 16.0f;
            }

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

        shapes[slot] = shape;
        computed[slot].store(true, std::memory_order_release);
        return shapes[slot];
    }

} // namespace Game
