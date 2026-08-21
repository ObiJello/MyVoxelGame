// File: src/common/world/block/BlockRegistry.cpp
#include "BlockRegistry.hpp"
#include "entity/DoubleChest.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/chunk/ChunkSection.hpp"   // g_blockRandomlyTicks
#include "BlockStateModels.hpp"
#include "GeneratedBlockShapes.hpp"
#include "GeneratedWaterlogged.hpp"
#include "Stairs.hpp"
#include "CrossCollision.hpp"
#include "Walls.hpp"
#include "Vine.hpp"
#include "MultifaceBlock.hpp"
#include "FenceGate.hpp"
#include "entity/BlockEntityTypes.hpp"
#include "../../core/Log.hpp"
#include <string_view>
#include <atomic>
#include <limits>
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {

    // Defined in BlockBehaviors.cpp — wires the per-block right-click callbacks
    // once the table exists. Mirrors ItemRegistry_RegisterBehaviors.
    void BlockRegistry_RegisterBehaviors(std::array<Block, BlockRegistry::Size>& blocks);
    // BlockGrowth.cpp — random-tick and bone-meal callbacks for the farming set.
    void BlockRegistry_RegisterGrowth(std::array<Block, BlockRegistry::Size>& blocks);

    namespace {

        // MC's own outline shape per BlockID, resolved from
        // GeneratedBlockShapes during Init. Only blocks whose MC shape is
        // state-INDEPENDENT appear (the generator skips getShapeForEachState
        // and friends), which is what makes one entry per BlockID sound.
        //
        // This exists because a block's MODEL is not its shape: a sapling is
        // drawn as `block/cross`, two planes spanning the whole cell, while
        // MC's SaplingBlock.SHAPE is a 12x12x12 column. Deriving the box from
        // the model gave saplings and mushrooms a full-cell hitbox.
        std::array<BlockRegistry::BlockShape, BlockRegistry::Size> s_mcShape{};
        std::array<bool, BlockRegistry::Size>                      s_hasMcShape{};

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

            // Nether wart is `.noCollision().randomTicks()` with no strength()
            // at all (Blocks.java:1683), so it takes Properties' 0.0 default.
            // Exact name, NOT a substring: "nether_wart" is a prefix of
            // nether_wart_block, which is a 1.0-strength hoe block.
            if (n == "nether_wart") {
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
                // Crops — all `.instabreak()` in Blocks.java (:1520, :1739,
                // :1740, :1963, :1960, :1961). "melon_stem"/"pumpkin_stem"
                // deliberately carry no leading underscore so they also match
                // attached_melon_stem / attached_pumpkin_stem, which are
                // instabreak too (:1661-1662). A bare "_stem" would wrongly
                // catch warped_stem / crimson_stem, which are 2.0 logs.
                "wheat", "carrots", "potatoes", "beetroots",
                "melon_stem", "pumpkin_stem", "pitcher_crop",
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

            // ── Farming blocks the name classifier can't reach ──────────────
            // Farmland matches no shovel substring ("farmland" contains none
            // of sand/gravel/_dirt/…), so it fell through to the 1.0 default.
            // Blocks.java:1521 — .strength(0.6F), and it is in #mineable/shovel.
            setHardness(BlockID::Farmland,     0.6f, ToolType::Shovel,  false, MiningTier::Wood);
            // Blocks.java:1592 — .strength(0.4F), no preferred tool.
            setHardness(BlockID::Cactus,       0.4f, ToolType::None,    false, MiningTier::Wood);
            // Blocks.java:1695 — .strength(0.2F, 3.0F), #mineable/axe.
            setHardness(BlockID::Cocoa,        0.2f, ToolType::Axe,     false, MiningTier::Wood);
            // Blocks.java:2089-2090 — both chain .instabreak() and THEN
            // .strength(1.0F); the later call wins, so neither is instant.
            // Both are #mineable/axe (and a sword one-shots bamboo, which the
            // engine has no rule for yet).
            setHardness(BlockID::Bamboo,        1.0f, ToolType::Axe,    false, MiningTier::Wood);
            setHardness(BlockID::BambooSapling, 1.0f, ToolType::Axe,    false, MiningTier::Wood);
        }


    } // anonymous namespace


    // Define the static array
    std::array<Block, BlockRegistry::Size> BlockRegistry::blockDefinitions{};

    // Declared in ChunkSection.hpp; defined here because Init() is what fills
    // it and this is the only translation unit that knows the answer.
    std::array<bool, static_cast<size_t>(BlockID::Count)> g_blockRandomlyTicks{};

    void BlockRegistry::RegisterModelBlock(BlockID id, const std::string& name, RenderLayer layer,
                                              const std::string& modelName,
                                              std::optional<bool> opaqueOverride) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterModelBlock", static_cast<unsigned>(id));
            return;
        }

        bool opaque = opaqueOverride.value_or(layer == RenderLayer::Opaque);
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

        // Override model names for blocks where minecraft ID != model file name
        RegisterModelBlock(BlockID::Water, "Water", RenderLayer::Translucent, "water_still");
        // Solid LAYER, non-occluding BLOCK — the two are separate in MC and
        // lava is the block where they disagree. ItemBlockRenderTypes
        // .LAYER_BY_FLUID registers WATER and FLOWING_WATER and nothing else,
        // so lava takes getRenderLayer's SOLID fallback; lava_still/lava_flow
        // have no alpha anywhere, and the translucent layer only bought lava a
        // per-frame back-to-front sort and an arbitrary order against glass and
        // water. It still occludes nothing, because LiquidBlock's occlusion
        // shape is empty — leaving `opaque` true here would delete the face of
        // every stone block touching a lava lake.
        RegisterModelBlock(BlockID::Lava, "Lava", RenderLayer::Opaque, "lava_still",
                           /*opaqueOverride=*/false);

        // ── Multipart blocks with no plain model of their own ───────────────
        // blockstates/tripwire.json dispatches on `attached` + four connection
        // booleans that this engine does not declare, so no variant matches and
        // the block falls back to its plain model — and there is no
        // models/block/tripwire.json, which left it an untextured full cube.
        // Point it at the no-connection variant vanilla uses for an isolated
        // string. Connections do not render; that needs the five properties.
        //
        // redstone_wire is deliberately NOT here. It is multipart too, but its
        // predicates read north/east/south/west, which ARE declared (see
        // StateKind::RedstoneWire) — so BlockStateModels matches the file,
        // unions the parts and synthesises a merged model per state. Giving it
        // a plain model here would BREAK that: a blockstate file is matched to
        // a block by model name, so renaming the model unlinks the file.
        RegisterModelBlock(BlockID::Tripwire, "Tripwire", RenderLayer::Cutout,
                           "tripwire_ns");

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


        // Bee nest — bare ID = honey_level<5 (empty visual). MC's
        // BlockModelGenerators.createBeeNest (line 791-798) shows only
        // honey_level=5 dispatches to the honey model; 0..4 all use empty.
        // Property: BeehiveBlock.HONEY_LEVEL = LEVEL_HONEY = IntegerProperty
        // "honey_level", 0..5 (BlockStateProperties.java:200).
        RegisterModelBlock(BlockID::BeeNest,      "Bee Nest",
                           RenderLayer::Opaque, "bee_nest_empty");

        // Leaf litter — bare ID = segment_amount=1. Variants 2..4 are
        // separate BlockIDs because MC's IntegerProperty SEGMENT_AMOUNT
        // (BlockStateProperties.java:165: 1..4) drives a different model per
        // value (template_leaf_litter_1..4 with progressively more visible
        // segment quads).
        RegisterModelBlock(BlockID::LeafLitter,  "Leaf Litter",
                           RenderLayer::Cutout, "leaf_litter_1");

        // Wildflowers — same `segment_amount` pattern as leaf litter.
        RegisterModelBlock(BlockID::Wildflowers,  "Wildflowers",
                           RenderLayer::Cutout, "wildflowers_1");

        // Pink petals — same shape but DIFFERENT property name. MC uses
        // `flower_amount` (BlockStateProperties.java:164 FLOWER_AMOUNT)
        // rather than leaf_litter/wildflowers' `segment_amount` (line 165).
        // Bare ID = flower_amount=1; variants 2..4 each have their own
        // model file with progressively more visible petal quads.
        RegisterModelBlock(BlockID::PinkPetals,  "Pink Petals",
                           RenderLayer::Cutout, "pink_petals_1");

        // Slab halves used to be registered here as their own BlockIDs, each
        // with its own model name. They are `type` states now, and
        // GetBlockModel(id, state) resolves them through BlockStateModels from
        // the block's own blockstate JSON — which is where vanilla keeps
        // "type=top" -> oak_slab_top and "type=double" -> oak_planks.

        // ── Registry slugs ──────────────────────────────────────────────────
        // Placed here deliberately: AFTER the last RegisterModelBlock (those
        // calls assign the WHOLE Block struct, so a slug written earlier is
        // lost to the model-name overrides) and BEFORE the first consumer.
        // BlockRegistry_RegisterBehaviors below matches blocks by
        // registrySlug — running this after it left every slug empty, so no
        // block got a use handler and even the crafting table stopped
        // opening. Column 2 of BlockDefs.inc IS the vanilla
        // registry name — the same string SectionDataUnpacker maps as
        // "minecraft:<m>" — and, unlike modelName, nothing rewrites it for
        // rendering. Data keyed on vanilla names (loot tables, recipes)
        // resolves through this and only this.
        #define BLOCK_DEF(e, m, d, r) \
            blockDefinitions[static_cast<size_t>(BlockID::e)].registrySlug = m;
        #include "BlockDefs.inc"
        #undef BLOCK_DEF

        // Promoted state variants have no BlockDefs.inc row of their own —
        // each stands in for ONE property value of a block that does (see
        // Blocks.hpp's manual section), so it shares that block's registry
        // name. Both halves of a double plant, both halves of a slab, snowy
        // and plain grass all resolve to one vanilla block, which is exactly
        // what MC does: they are BlockStates of a single Block. Mirrors the
        // state keys in SectionDataUnpacker::BlockStateRegistry::Initialize.

        // ── MC outline shapes, resolved slug -> BlockID once ───────────────
        // GeneratedBlockShapes carries BlockBehaviour.getShape for every block
        // whose shape is a plain static VoxelShape in MC's source. Resolving
        // here rather than looking up by string per query keeps GetBlockShape
        // an array read, and it has to be AFTER the slug passes above — the
        // table is keyed on registrySlug.
        {
            std::unordered_map<std::string_view, const GeneratedBlockShapeRow*> bySlug;
            bySlug.reserve(kBlockShapeTableSize);
            for (size_t i = 0; i < kBlockShapeTableSize; ++i) {
                bySlug.emplace(kBlockShapeTable[i].slug, &kBlockShapeTable[i]);
            }
            size_t matched = 0;
            for (size_t i = 0; i < blockDefinitions.size(); ++i) {
                const std::string& slug = blockDefinitions[i].registrySlug;
                if (slug.empty()) continue;
                auto it = bySlug.find(slug);
                if (it == bySlug.end()) continue;
                const GeneratedBlockShapeRow& r = *it->second;
                s_mcShape[i]    = BlockShape{ glm::vec3(r.minX, r.minY, r.minZ),
                                              glm::vec3(r.maxX, r.maxY, r.maxZ) };
                s_hasMcShape[i] = true;
                ++matched;
            }
            Log::Info("MC block shapes - %zu of %zu table entries matched a block",
                      matched, kBlockShapeTableSize);
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
            // MC buttonProperties() is `.noCollision()` — you walk straight
            // through a button. It was already in the instant-break list but
            // not this one, so buttons were solid.
            "_button",
            // Crops — every one is `.noCollision()` in Blocks.java. Same
            // stem-naming caution as kInstantSubstr above. Cactus, bamboo and
            // cocoa are deliberately absent: MC gives all three a real
            // collision shape.
            "wheat", "carrots", "potatoes", "beetroots",
            "melon_stem", "pumpkin_stem", "pitcher_crop", "sweet_berry_bush",
            // Tall two-block flowers — all .noCollision() in Blocks.java, and
            // none of them has a potted variant to trip over.
            "rose_bush", "peony", "lilac", "sunflower",
            // ── Added after diffing this table against every `.noCollision()`
            // in Blocks.java. Each of these was SOLID here and is walk-through
            // in MC, which does not just block movement: MoveControl's
            // auto-jump fires whenever a mob's own cell holds a block whose
            // collision top is above its feet, so standing in one made a mob
            // bounce on the spot every tick. That is what a sheep in a savanna
            // was doing — short_dry_grass is ground cover there.
            "short_dry_grass", "tall_dry_grass",
            "_pressure_plate",          // MC pressurePlateProperties() is .noCollision()
            "blue_orchid",              // a flower whose name has no "_flower"
            "small_dripleaf", "big_dripleaf_stem", "mangrove_propagule",
            "pale_hanging_moss", "frogspawn", "nether_sprouts",
            "scaffolding",              // Blocks.java: .noCollision() (its own shape is the climb)
            "cobweb",                   // slows you down; never blocks you
        };
        // ── Support: blocks whose MC canSurvive is "solid top face below" ──
        // A strict SUBSET of the noCollision list above, and the difference is
        // the whole point: `vine`, `glow_lichen`, `sculk_vein`,
        // `hanging_roots`, `spore_blossom`, `weeping_vines` and the wall
        // torch/sign/banner variants also have no collision, but they hang off
        // a SIDE or a CEILING. Including them here would delete them whenever
        // the block under them changed.
        //
        // `kelp` and `seagrass` are out for a different reason — their MC rule
        // is about water, which this engine does not simulate.
        static constexpr std::string_view kNeedsSupportBelowSubstr[] = {
            "_sapling", "_flower", "tulip", "allium", "azure_bluet",
            "oxeye_daisy", "cornflower", "lily_of_the_valley", "dandelion",
            "poppy", "wither_rose", "torchflower", "open_eyeblossom",
            "closed_eyeblossom", "pitcher_plant",
            "short_grass", "tall_grass", "fern", "large_fern", "dead_bush",
            "leaf_litter", "wildflowers", "pink_petals",
            "warped_roots", "crimson_roots", "warped_fungus", "crimson_fungus",
            "sugar_cane",
            // MC RedStoneWireBlock.updateShape returns AIR when the block
            // below can no longer hold dust, so mining out its support breaks
            // it. CanSurviveOn carries the real rule (sturdy face, or hopper).
            "redstone_wire",
            // Crops. VegetationBlock.canSurvive is `mayPlaceOn(stateBelow)`,
            // so mining the farmland out from under wheat breaks it. Cocoa is
            // out — it attaches to a jungle log on a SIDE, and listing it here
            // would delete every cocoa pod whenever the block below changed.
            "wheat", "carrots", "potatoes", "beetroots",
            "melon_stem", "pumpkin_stem", "pitcher_crop", "sweet_berry_bush",
        };
        auto needsSupportBelowFor = [](const std::string& n) -> bool {
            // Nether wart sits on soul sand and breaks when that goes, but
            // "nether_wart" is a prefix of nether_wart_block — a full cube
            // that must not acquire a support rule. Exact match only.
            if (n == "nether_wart") return true;
            // "weeping_vines" hangs DOWN from a ceiling, so it must not match
            // through any of the entries above; none of them are substrings of
            // it, but keep the guard explicit since the flower list is broad.
            if (n.find("weeping_vines") != std::string::npos) return false;
            for (auto sv : kNeedsSupportBelowSubstr) {
                if (n.find(sv) != std::string::npos) return true;
            }
            return false;
        };

        // ── MC `.replaceable()` (BlockBehaviour.Properties) ─────────────────
        //
        // Extracted verbatim from every `.replaceable()` call in Blocks.java.
        // This is the flag that lets you place a block straight into water, or
        // into tall grass, instead of the placement being refused.
        //
        // Matched on registrySlug, NOT the model name, and by EXACT equality.
        // Both matter here: half of this list is a substring of something that
        // must NOT be replaceable — "vine" is inside weeping_vines / cave_vines,
        // "fire" inside campfire and fire_coral_block, "snow" inside snow_block
        // and powder_snow, "light" inside lightning_rod and light_blue_wool,
        // "bush" inside sweet_berry_bush. Slugs also come free with the right
        // answer for promoted state variants: both halves of tall grass carry
        // the base block's slug, so listing it once covers them.
        //
        // Notable absences, all deliberate and all matching vanilla: kelp,
        // sweet_berry_bush, powder_snow, snow_block, and every solid block.
        static constexpr std::string_view kReplaceableSlugs[] = {
            "water", "lava", "bubble_column",
            "short_grass", "fern", "tall_grass", "large_fern",
            "short_dry_grass", "tall_dry_grass",
            "dead_bush", "bush",
            "seagrass", "tall_seagrass",
            "fire", "soul_fire",
            "snow",                       // the LAYER; snow_block is not
            "vine", "glow_lichen", "resin_clump",
            "light", "structure_void",
            "warped_roots", "crimson_roots", "nether_sprouts", "hanging_roots",
            "leaf_litter",
        };
        auto replaceableFor = [](const std::string& slug) -> bool {
            for (auto sv : kReplaceableSlugs) if (slug == sv) return true;
            return false;
        };

        auto noCollisionFor = [](const std::string& n) -> bool {
            // Flower pots KEEP their collision — MC flowerPotProperties()
            // (Blocks.java:1266) has no .noCollision(), and FlowerPotBlock has
            // a real 6x6x6 shape you stand on.
            //
            // This guard has to run FIRST because every potted_* name embeds
            // the plant it holds: "potted_dead_bush" contains "dead_bush", so
            // the substring scan below was already stripping collision from
            // potted plants before "bush" was ever added to it.
            if (n.rfind("potted_", 0) == 0) return false;

            // Exact-match cases that must NOT go in the substring table above:
            // "fire" as a substring would also catch fire_coral_block (a solid
            // cube) and campfire, both of which keep their collision.
            // Blocks.java:1509-1510 — fire and soul_fire are .noCollision().
            if (n == "fire" || n == "soul_fire") return true;
            // Same nether_wart / nether_wart_block prefix trap as above.
            if (n == "nether_wart") return true;
            // Blocks.java:1447 — BUSH is .noCollision(). Exact match: as a
            // substring it would reach azalea_bush and friends, and the
            // potted_ guard above is the only thing that would save them.
            if (n == "bush") return true;
            // Mushrooms are .noCollision(); the *_mushroom_block and
            // mushroom_stem cubes are not, and both embed the same name.
            if (n == "brown_mushroom" || n == "red_mushroom") return true;
            // The bare "rail" — the "_rail" substring above catches the
            // powered/detector/activator variants but not this one.
            if (n == "rail") return true;
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
            b.needsSupportBelow   = needsSupportBelowFor(name);
            // Slug, not model name — see kReplaceableSlugs. Safe here because
            // the registry-slug pass above has already run.
            b.replaceable         = replaceableFor(b.registrySlug);
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

        // Right-click behaviour (crafting table's menu, container menus, …).
        // Must come after the registry-slug pass above: it looks blocks up by
        // registrySlug, and an empty slug matches nothing.
        BlockRegistry_RegisterBehaviors(blockDefinitions);

        // Must run after every block is registered — it classifies from the
        // registered model names.
        InitBlockStates();

        // Growth callbacks. Strictly AFTER InitBlockStates: every one of them
        // reads its block's `age` / `moisture` property to decide what to do,
        // and the registration pass itself derives max age from the state
        // definition. Wiring them before the definitions exist would give every
        // crop a max age of 0 and nothing would ever grow.
        BlockRegistry_RegisterGrowth(blockDefinitions);

        // Publish the flat "does this block random-tick" table ChunkSection
        // consults on every write. Must follow RegisterGrowth, which is what
        // decides the answer. Re-running Init (world reload) simply refills it.
        {
            size_t ticking = 0;
            for (size_t i = 0; i < blockDefinitions.size(); ++i) {
                const bool ticks = blockDefinitions[i].randomTick != nullptr;
                g_blockRandomlyTicks[i] = ticks;
                if (ticks) ++ticking;
            }
            Log::Info("Random ticking - %zu of %zu blocks take random ticks",
                      ticking, blockDefinitions.size());
        }

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

    const BlockModel& BlockRegistry::GetBlockModel(BlockState state) {
        const BlockID id = state.Block();
        const std::string& stateModel = BlockStateModels::ModelNameFor(id, state.Index());
        if (!stateModel.empty()) {
            return BlockModelRegistry::GetModel(stateModel);
        }
        return GetBlockModel(id);
    }

    bool BlockRegistry::HasCollision(BlockID id) {
        // The *SlabTop/*SlabDouble special case that used to sit here is gone
        // with those BlockIDs. All three halves are one block now, and the slab
        // classifies as collidable once — so there is no second id that could
        // be classified differently from the one the player walks on.
        return Get(id).hasCollision;
    }

    // ── Slabs (MC SlabBlock) ────────────────────────────────────────────────
    //
    // The half is the `type` blockstate now, not a BlockID. What used to live
    // here — three BlockID-indexed tables built from kSlabPairs, plus the
    // SlabTopVariant/SlabBottomVariant/SlabDoubleVariant trio — is gone with
    // the *SlabTop/*SlabDouble ids they mapped between.
    //
    // Those tables were also the source of a shipped bug worth remembering:
    // every one was FILLED with BlockID::Air as its "not a slab" sentinel, so
    // at index 0 the entry and the key were both Air and `table[i] == id` said
    // yes for AIR ITSELF. HasCollision consulted it, so the whole sky went
    // solid — the player could not move, and the heightmap (also built on
    // HasCollision) put spawn at the build limit. Reading a property instead of
    // a sentinel table removes the entire failure mode.

    bool BlockRegistry::IsSlabBlock(BlockID id) {
        // A slab is exactly a block carrying MC's SlabBlock.TYPE. No name
        // matching, no table: the state definition is the authority.
        return BlockStates::Default(id).HasProperty(PropertyId::SLAB_TYPE);
    }

    BlockRegistry::SlabType BlockRegistry::SlabTypeOf(BlockState state) {
        const int v = state.GetIndex(PropertyId::SLAB_TYPE);
        if (v < 0) return SlabType::NotSlab;
        // MC's value order for SlabType is top, bottom, double.
        switch (v) {
            case 0:  return SlabType::Top;
            case 1:  return SlabType::Bottom;
            default: return SlabType::Double;
        }
    }

    BlockState BlockRegistry::SlabStateWithType(BlockState state, SlabType type) {
        if (type == SlabType::NotSlab) return state;
        const int v = (type == SlabType::Top) ? 0 : (type == SlabType::Bottom) ? 1 : 2;
        return state.SetIndex(PropertyId::SLAB_TYPE, v);
    }

    BlockID BlockRegistry::SlabFamilyBase(BlockID id) {
        return IsSlabBlock(id) ? id : BlockID::Air;
    }

    std::string_view BlockRegistry::ImpliedPropertyValue(BlockID id, std::string_view propName) {
        // Nothing is implied any more. This existed because the slab halves
        // were separate BlockIDs, so MC's data files could ask a *SlabTop about
        // `type` and there was no property to answer with. `type` is a real
        // blockstate now and BlockStateModels reads it directly, so the hook
        // stays only for the next family that needs it (the segmented clumps
        // still spend an id per `segment_amount`).
        (void)id; (void)propName;
        return {};
    }

    // ========================================================================
    // BLOCK STATES  (MC StateDefinition / createBlockStateDefinition)
    // ========================================================================

    // The four methods below are now thin wrappers over BlockState, which is
    // where the arithmetic actually lives. They exist so the ~190 call sites
    // that still speak (BlockID, index) keep working while they are converted;
    // each one is exactly MC's equivalent operation on `defaultBlockState()`.

    uint16_t BlockRegistry::BlockStateDefinition::StateCount() const {
        return static_cast<uint16_t>(BlockStates::Count(owner));
    }

    uint16_t BlockRegistry::BlockStateDefinition::IndexOf(const PropertyMap& props) const {
        // MC NbtUtils.readBlockState: start from the block's DEFAULT state and
        // set only the properties actually supplied, skipping anything unknown.
        // Starting from index 0 instead — as this did while "0 == default" held
        // — would now hand back `waterlogged=true, powered=true, lit=true` for
        // everything the caller did not mention.
        BlockState s = BlockStates::Default(owner);
        for (const auto& prop : properties) {
            auto it = props.find(prop.name);
            if (it == props.end()) continue;
            s = s.SetName(prop.id, it->second);
        }
        return static_cast<uint16_t>(s.RawId() - BlockStates::Base(owner));
    }

    BlockRegistry::BlockStateDefinition::PropertyMap
    BlockRegistry::BlockStateDefinition::PropertiesOf(uint16_t stateIndex) const {
        PropertyMap out;
        const BlockState s = BlockState::FromRawId(BlockStates::Base(owner) + stateIndex);
        for (const auto& prop : properties) {
            out[prop.name] = std::string(s.GetName(prop.id));
        }
        return out;
    }

    std::string_view BlockRegistry::BlockStateDefinition::ValueOf(
            uint16_t stateIndex, std::string_view propName) const {
        for (const auto& prop : properties) {
            if (prop.name != propName) continue;
            const BlockState s = BlockState::FromRawId(BlockStates::Base(owner) + stateIndex);
            return s.GetName(prop.id);
        }
        return {};
    }

    uint16_t BlockRegistry::BlockStateDefinition::IndexOfSingle(
            std::string_view propName, std::string_view value) const {
        BlockState s = BlockStates::Default(owner);
        for (const auto& prop : properties) {
            if (prop.name != propName) continue;
            s = s.SetName(prop.id, value);
            break;
        }
        return static_cast<uint16_t>(s.RawId() - BlockStates::Base(owner));
    }

    namespace {
        // One entry per BlockID; blocks with no properties keep an empty
        // definition (a single state, index 0). Built once in Init().
        std::array<BlockRegistry::BlockStateDefinition, BlockRegistry::Size> s_stateDefs{};

        // Per-BlockID waterlogging facts, filled by InitBlockStates from the
        // generated tables. Flat and POD because the mesher asks about the
        // block AND all six neighbours of every fluid voxel it touches.
        struct WaterlogInfo {
            bool waterloggable = false;  // declares the `waterlogged` property
            bool defaultTrue   = false;  // registerDefaultState installs `true`
            bool alwaysWater   = false;  // getFluidState is unconditionally WATER
        };
        std::array<WaterlogInfo, BlockRegistry::Size> s_waterlog{};

        // Generated-table lookups, built once. Keyed on the VANILLA registry
        // slug, not the model name: a slab's top half is its own BlockID here
        // with model `oak_slab_top`, and MC knows it only as `oak_slab`.
        const std::unordered_map<std::string_view, bool>& kWaterloggable() {
            static const std::unordered_map<std::string_view, bool> m = [] {
                std::unordered_map<std::string_view, bool> t;
                t.reserve(kWaterloggableTableSize * 2);
                for (size_t i = 0; i < kWaterloggableTableSize; ++i) {
                    t.emplace(kWaterloggableTable[i].slug,
                              kWaterloggableTable[i].defaultWaterlogged);
                }
                return t;
            }();
            return m;
        }

        const std::unordered_set<std::string_view>& kAlwaysWaterlogged() {
            static const std::unordered_set<std::string_view> m = [] {
                std::unordered_set<std::string_view> t;
                for (size_t i = 0; i < kAlwaysWaterloggedTableSize; ++i) {
                    t.emplace(kAlwaysWaterloggedTable[i]);
                }
                return t;
            }();
            return m;
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

    // Build every block's property list from the generated tables.
    //
    // This replaced ~600 lines of hand-written value tables and a `StateKind`
    // classifier that guessed a block's properties from its MODEL NAME. Three
    // things were wrong with that and none of them were fixable by care:
    //
    //   * it only knew the families someone had written out — a couple of
    //     dozen, against the 762 blocks that actually carry properties;
    //   * the value ORDER was guessed, and MC's is not what you would guess
    //     (`facing` is north,south,west,east; `half` is top,bottom);
    //   * it keyed properties by NAME, and `type` alone means three different
    //     things depending on the block.
    //
    // See tools/gen_block_states.py. The data is checked against vanilla by
    // tools/verify_block_states.py and the runtime tables by
    // tools/blockstate_parity.
    void BlockRegistry::InitBlockStates() {
        BlockStates::Init();

        for (size_t i = 0; i < Size; ++i) {
            const BlockID id = static_cast<BlockID>(i);
            BlockStateDefinition def;
            def.owner        = id;
            def.defaultIndex = static_cast<uint16_t>(
                BlockStates::Default(id).RawId() - BlockStates::Base(id));

            const uint16_t n = BlockStates::PropertyCount(id);
            def.properties.reserve(n);
            for (uint16_t slot = 0; slot < n; ++slot) {
                const PropertyId p = BlockStates::PropertyAt(id, slot);
                BlockStateDefinition::Property prop;
                prop.id   = p;
                prop.name = std::string(BlockStates::PropertyName(p));
                const uint16_t vc = BlockStates::PropertyValueCount(p);
                prop.values.reserve(vc);
                for (uint16_t v = 0; v < vc; ++v) {
                    prop.values.emplace_back(BlockStates::PropertyValueName(p, v));
                }
                def.properties.push_back(std::move(prop));
            }

            // `waterlogged` is now just another property, found by identity
            // rather than by a name-keyed table. The old low-bit mask that
            // ContainsWater/WithWaterlogged used is gone with it: under MC's
            // sorted-by-name property order `waterlogged` is NOT the trailing
            // digit for every block — a wall sorts east,north,south,up,
            // waterlogged,west — so the mask was about to start reading `west`.
            s_waterlog[i].waterloggable = false;
            for (const auto& prop : def.properties) {
                if (prop.id == PropertyId::WATERLOGGED) {
                    s_waterlog[i].waterloggable = true;
                    break;
                }
            }
            const std::string& slug = blockDefinitions[i].registrySlug;
            s_waterlog[i].alwaysWater =
                !slug.empty() && kAlwaysWaterlogged().count(slug) > 0;

            s_stateDefs[i] = std::move(def);
        }

        // AIR must not have acquired properties, and must still be state id 0 —
        // the palette fill, a zero-initialised voxel and BlockState{} all mean
        // air only because of that. Checked rather than assumed: this codebase
        // has already shipped one bug where air was quietly classified as a
        // real block and the whole sky turned solid.
        if (BlockStates::Count(BlockID::Air) != 1 ||
            BlockStates::Default(BlockID::Air).RawId() != 0 ||
            !s_stateDefs[static_cast<size_t>(BlockID::Air)].properties.empty()) {
            Log::Error("AIR has acquired block states - every empty voxel in the "
                       "world is about to mean something else");
        }

        size_t stateful = 0, waterloggable = 0, alwaysWater = 0;
        for (const auto& d : s_stateDefs) if (!d.properties.empty()) ++stateful;
        for (const auto& w : s_waterlog) {
            if (w.waterloggable) ++waterloggable;
            if (w.alwaysWater)   ++alwaysWater;
        }
        Log::Info("Block states initialized - %zu of %zu blocks carry state properties, "
                  "%u states total (%d bits)",
                  stateful, static_cast<size_t>(BlockID::Count),
                  BlockStates::Total(), kBlockStateBits);
        Log::Info("Waterlogging - %zu blocks declare `waterlogged`, %zu are always water",
                  waterloggable, alwaysWater);
    }


    bool BlockRegistry::IsWaterloggable(BlockID id) {
        const size_t i = static_cast<size_t>(id);
        return i < Size && s_waterlog[i].waterloggable;
    }

    bool BlockRegistry::IsAlwaysWaterlogged(BlockID id) {
        const size_t i = static_cast<size_t>(id);
        return i < Size && s_waterlog[i].alwaysWater;
    }

    // Both of these used to do bit tricks on the state index, valid only while
    // `waterlogged` was the TRAILING property of every block that declares it.
    // Under MC's sorted-by-name property order it is not: a wall sorts
    // east, north, south, up, waterlogged, west — so the low bit is `west`, and
    // the mask was one rename away from reading the wrong property and one
    // block away from truncating at 256. Read the property instead.
    bool BlockRegistry::ContainsWater(BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Water) return true;
        const size_t i = static_cast<size_t>(id);
        if (i >= Size) return false;
        const WaterlogInfo& w = s_waterlog[i];
        if (w.alwaysWater) return true;
        if (!w.waterloggable) return false;
        // Booleans list [true, false], so index 0 is true.
        return state.GetIndex(PropertyId::WATERLOGGED) == 0;
    }

    BlockState BlockRegistry::WithWaterlogged(BlockState state, bool on) {
        const size_t i = static_cast<size_t>(state.Block());
        if (i >= Size || !s_waterlog[i].waterloggable) return state;
        return state.SetIndex(PropertyId::WATERLOGGED, on ? 0 : 1);
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
        // The flat (BlockID, stateIndex) -> id table, shared by the shape cache
        // and by Game::BlockStateIds. One slot per STATE, not per block.
        //
        // State count is captured AT BUILD TIME. Indices must be clamped
        // against this, never against a freshly-read StateCount(): the table is
        // sized once, so if anything queries before InitBlockStates has run
        // every block is one state wide, and clamping against the later, larger
        // count would index straight out of a block's slice and into the next
        // one's.
        // The flat (BlockID, stateIndex) -> global id mapping.
        //
        // This used to be a second table built independently of the block state
        // registry, and it CLAMPED every block to 256 states because the index
        // was a uint8_t. That clamp is gone with the widening, and so is the
        // table: BlockStates already owns exactly this mapping, and two tables
        // that can disagree about how many states a block has is precisely the
        // kind of split-brain that shows up as a chunk palette sized for a
        // different world than the one being written into it.
        struct StateIdTableView {
            uint32_t base(size_t i) const {
                return BlockStates::Base(static_cast<BlockID>(i));
            }
            uint32_t count(size_t i) const {
                return BlockStates::Count(static_cast<BlockID>(i));
            }
            uint32_t total_() const { return BlockStates::Total(); }
        };

        struct StateIdsProxy {
            uint32_t total;
            int      bits;
            StateIdTableView view;
            uint32_t baseAt(size_t i)  const { return view.base(i); }
            uint32_t countAt(size_t i) const { return view.count(i); }
        };

        StateIdsProxy StateIds() {
            return StateIdsProxy{BlockStates::Total(), kBlockStateBits, {}};
        }

        struct StateShapeCache {
            std::vector<BlockRegistry::BlockShape> shapes;
            std::unique_ptr<std::atomic<bool>[]>   computed;

            StateShapeCache() {
                const uint32_t total = StateIds().total;
                shapes.assign(total, BlockRegistry::BlockShape{});
                computed = std::make_unique<std::atomic<bool>[]>(total);
                for (uint32_t k = 0; k < total; ++k) {
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

        // The same cache for the box-union form. Kept separate rather than
        // folded into StateShapeCache because GetBlockShape hands out a
        // reference to its entry and the two are filled on different paths —
        // a set entry is only ever built for a block whose shape is not one
        // box, and every other block's set is derived from the single shape.
        struct StateShapeSetCache {
            std::vector<BlockRegistry::BlockShapeSet> sets;
            std::unique_ptr<std::atomic<bool>[]>      computed;

            StateShapeSetCache() {
                const uint32_t total = StateIds().total;
                sets.assign(total, BlockRegistry::BlockShapeSet{});
                computed = std::make_unique<std::atomic<bool>[]>(total);
                for (uint32_t k = 0; k < total; ++k) {
                    computed[k].store(false, std::memory_order_relaxed);
                }
            }
        };

        StateShapeSetCache& StateShapeSets() {
            static StateShapeSetCache c;
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

        // ── Per-age crop shapes ─────────────────────────────────────────────
        //
        // Every one of these is `Block.column(widthPx, 0, baseHeightPx + age *
        // heightStepPx)` in the block's own class, quoted below. MC keeps them
        // in a `SHAPES` array built by Block.boxes(maxAge, …); we recompute the
        // one entry asked for, which is the same arithmetic without the table.
        struct CropShapeRule {
            BlockID id;
            float   widthPx;
            int     baseHeightPx;
            int     heightStepPx;
            bool    readsAge;   // false for the flat-shaped attached stems
        };
        constexpr CropShapeRule kCropShapeRules[] = {
            // CropBlock.java:180      — column(16, 0, 2 + age * 2)
            { BlockID::Wheat,           16.0f, 2, 2, true },
            // BeetrootBlock.java:41    — same formula, max age 3
            { BlockID::Beetroots,       16.0f, 2, 2, true },
            // CarrotBlock / PotatoBlock — column(16, 0, 2 + age), a flatter crop
            { BlockID::Carrots,         16.0f, 2, 1, true },
            { BlockID::Potatoes,        16.0f, 2, 1, true },
            // NetherWartBlock.java:36  — column(16, 0, 5 + age * 3)
            { BlockID::NetherWart,      16.0f, 5, 3, true },
            // StemBlock.java:73        — column(2, 0, 2 + age * 2): a thin stalk
            { BlockID::MelonStem,        2.0f, 2, 2, true },
            { BlockID::PumpkinStem,      2.0f, 2, 2, true },
            // TorchflowerCropBlock     — column(6, 0, 6 + age * 4)
            { BlockID::TorchflowerCrop,  6.0f, 6, 4, true },
            // SugarCaneBlock           — column(12, 0, 16), no age term
            { BlockID::SugarCane,       12.0f, 16, 0, false },
        };

        const CropShapeRule* CropShapeRuleFor(BlockID id) {
            for (const auto& r : kCropShapeRules) if (r.id == id) return &r;
            return nullptr;
        }

        // ── Attached stem ───────────────────────────────────────────────────
        //
        // The one farming shape that is NOT a centred column, so it can't ride
        // the table above. MC AttachedStemBlock:
        //
        //     SHAPES = Shapes.rotateHorizontal(Block.boxZ(4, 0, 10, 0, 10));
        //
        // which for facing=north is the pixel box (6,0,0)-(10,10,10): a 4-wide,
        // 10-tall bar running from the block's north edge to just past centre —
        // i.e. reaching TOWARD the fruit it is attached to. The other three
        // facings are that box rotated about Y.
        //
        // This matters twice over. The obvious half is the hitbox: without it
        // the shape falls through to the model union of `block/stem_fruit`,
        // whose quads span the whole cell, and a mature stem gets a full-block
        // selection box. The subtler half is that the mesher's occlusion test
        // is `block.opaque && fullCube` — so a full-cube shape ALSO made the
        // stem cull the face of the melon or pumpkin beside it.
        bool AttachedStemShape(BlockID id, BlockStateIndex stateIndex,
                               BlockRegistry::BlockShape& out) {
            if (id != BlockID::AttachedMelonStem && id != BlockID::AttachedPumpkinStem) {
                return false;
            }
            const std::string_view facing =
                BlockRegistry::GetStateDefinition(id).ValueOf(stateIndex, "facing");

            // Pixel extents, north as authored; the rest mirrored/swapped.
            float x0 = 6.0f, x1 = 10.0f, z0 = 0.0f, z1 = 10.0f;
            if (facing == "south")      { z0 =  6.0f; z1 = 16.0f; }
            else if (facing == "west")  { x0 =  0.0f; x1 = 10.0f; z0 = 6.0f; z1 = 10.0f; }
            else if (facing == "east")  { x0 =  6.0f; x1 = 16.0f; z0 = 6.0f; z1 = 10.0f; }
            // "north" (and any unrecognised value) keeps the authored box.

            out = BlockRegistry::BlockShape{
                glm::vec3(x0 / 16.0f, 0.0f,          z0 / 16.0f),
                glm::vec3(x1 / 16.0f, 10.0f / 16.0f, z1 / 16.0f)
            };
            return true;
        }

        // ── Buttons and levers ──────────────────────────────────────────────
        //
        // Their MC shape is NOT their model's bounding box, and gen_block_shapes
        // skips them because it is per-state. A lever's model carries a 10-pixel
        // handle rotated 45 degrees, so the model-derived union came out roughly
        // twice the height of the real hitbox and offset by the handle's lean.
        //
        // MC authors both shapes in the WALL-NORTH frame:
        //   ButtonBlock  boxZ(6, 4, 8, 16) minus the unpressed/pressed cube
        //   LeverBlock   boxZ(6, 8, 10, 16)
        // and rotates with Shapes.rotateAttachFace. Those are converted here
        // into the FLOOR frame instead, so the same (x, y) quarter-turns the
        // blockstate JSON applies to the MODEL also apply to the shape — which
        // keeps hitbox and geometry consistent by construction rather than by
        // two tables agreeing.
        //
        // The conversion is verified against the model files: un-rotating the
        // button's wall-north shape by x:270 gives exactly (5,0,6)-(11,2,10),
        // which is block/button.json's element, and the pressed variant gives
        // (5,0,6)-(11,1,10), matching button_pressed.json.
        struct FaceAttachedShape { float x0, y0, z0, x1, y1, z1; };

        // (xTurns, yTurns) per face/facing, straight from the blockstate JSONs
        // (identical for oak_button and lever). Note the CEILING row is offset
        // by two: flipping about X reverses the horizontal sense, so vanilla
        // lists south at y=0 there rather than north.
        struct FaceAttachedTurns { uint8_t xTurns, yTurns; };
        FaceAttachedTurns FaceAttachedTurnsFor(std::string_view face, std::string_view facing) {
            const uint8_t f = (facing == "east")  ? 1
                            : (facing == "south") ? 2
                            : (facing == "west")  ? 3 : 0;
            if (face == "floor")   return { 0, f };
            if (face == "ceiling") return { 2, static_cast<uint8_t>((f + 2) & 3) };
            return { 1, f };   // wall
        }

        // Rotate a pixel-space AABB about the block centre, matching
        // BlockModel's RotX90 / RotY90 exactly (see the sign note there).
        BlockRegistry::BlockShape RotateAabbPixels(const FaceAttachedShape& b,
                                                   int xTurns, int yTurns) {
            auto rot = [&](glm::vec3 p) {
                glm::vec3 c = p - glm::vec3(8.0f);
                for (int i = 0; i < xTurns; ++i) c = glm::vec3(c.x,  c.z, -c.y);
                for (int i = 0; i < yTurns; ++i) c = glm::vec3(-c.z, c.y,  c.x);
                return c + glm::vec3(8.0f);
            };
            const glm::vec3 a = rot({b.x0, b.y0, b.z0});
            const glm::vec3 d = rot({b.x1, b.y1, b.z1});
            return BlockRegistry::BlockShape{ glm::min(a, d) / 16.0f,
                                              glm::max(a, d) / 16.0f };
        }

        // Returns false when the block is not a button or a lever.
        bool FaceAttachedShapeFor(BlockID id, BlockStateIndex stateIndex,
                                  BlockRegistry::BlockShape& out) {
            const std::string& n = BlockRegistry::Get(id).modelName;
            const bool isButton = n.find("_button") != std::string::npos;
            const bool isLever  = (n == "lever");
            if (!isButton && !isLever) return false;

            const auto& def = BlockRegistry::GetStateDefinition(id);
            const std::string_view face    = def.ValueOf(stateIndex, "face");
            const std::string_view facing  = def.ValueOf(stateIndex, "facing");
            const bool             powered = def.ValueOf(stateIndex, "powered") == "true";

            const FaceAttachedShape box =
                isLever  ? FaceAttachedShape{5, 0, 4, 11, 6, 12}
                : powered ? FaceAttachedShape{5, 0, 6, 11, 1, 10}
                          : FaceAttachedShape{5, 0, 6, 11, 2, 10};

            const FaceAttachedTurns t = FaceAttachedTurnsFor(face, facing);
            out = RotateAabbPixels(box, t.xTurns, t.yTurns);
            return true;
        }

        // The `age` property as an integer. Kept local rather than shared with
        // BlockGrowth.cpp's copy: this one runs inside the shape cache, on the
        // mesher's threads, and must not pull the growth header in.
        int AgeFromState(BlockID id, BlockStateIndex stateIndex) {
            const std::string_view v =
                BlockRegistry::GetStateDefinition(id).ValueOf(stateIndex, "age");
            int n = 0;
            for (char c : v) {
                if (c < '0' || c > '9') return 0;
                n = n * 10 + (c - '0');
            }
            return n;
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

    BlockRegistry::BlockShape BlockRegistry::GetBlockShapeAt(
            const IBlockAccess& world, const glm::ivec3& pos, BlockState state) {
        const BlockID id = state.Block();
        BlockShape shape = GetBlockShape(state);
        if (id != BlockID::Chest && id != BlockID::TrappedChest) return shape;

        const auto pair = FindChestPartner(world, pos);
        if (!pair) return shape;

        // MC ChestBlock.java:321 — HALF_SHAPES is the single column
        // (Block.column(14,0,14)) extended to the cell edge on the CONNECTED
        // side: boxZ(14, 0,14, 0,15) reaches z=0 for a north connection, and
        // Shapes.rotateHorizontal spins that for the other three. Without the
        // extension each half stays inset by a pixel, leaving a 2px dead strip
        // down the middle of a double chest that swallows clicks — a
        // right-click there hits nothing and a placement lands on the block
        // behind.
        const glm::ivec3 d = pair->partnerPos - pos;
        if      (d.x > 0) shape.max.x = 1.0f;
        else if (d.x < 0) shape.min.x = 0.0f;
        else if (d.z > 0) shape.max.z = 1.0f;
        else if (d.z < 0) shape.min.z = 0.0f;
        return shape;
    }

    namespace {
        // Which families build their shape from a box union rather than from
        // the model. Each one is a MC class that overrides getShape.
        bool HasMultiBoxShape(BlockID id) {
            return IsStairs(id) || IsCrossCollisionBlock(id) ||
                   IsWallBlock(id) || IsFenceGateBlock(id) ||
                   IsVineBlock(id) || IsMultifaceBlock(id);
        }

        // `collision` picks getCollisionShape over getShape. They differ for
        // the fence, wall and gate families; a stair's are the same shape.
        BlockRegistry::BlockShapeSet MultiBoxShape(BlockState state,
                                                   bool collision) {
            const BlockID id = state.Block();
            if (IsVineBlock(id))          return VineShapeBoxes(state);
            if (IsMultifaceBlock(id))     return MultifaceShapeBoxes(state);
            if (IsStairs(id))             return StairShapeBoxes(state);
            if (IsCrossCollisionBlock(id)) return collision ? CrossCollisionBoxes(state)
                                                            : CrossShapeBoxes(state);
            if (IsWallBlock(id))          return collision ? WallCollisionBoxes(state)
                                                           : WallShapeBoxes(state);
            return collision ? FenceGateCollisionBoxes(state)
                             : FenceGateShapeBoxes(state);
        }
    } // namespace

    BlockRegistry::BlockShapeSet BlockRegistry::GetBlockShapeSetAt(
            const IBlockAccess& world, const glm::ivec3& pos, BlockState state) {
        const BlockID id = state.Block();
        BlockShapeSet set = GetBlockShapeSet(state);
        // The only world-aware shape is the paired chest, and a chest is a
        // single box — so the extension applies to box 0 and nothing else in
        // the set can be affected. Routed through GetBlockShapeAt rather than
        // repeating its arithmetic so the two cannot drift.
        if (id == BlockID::Chest || id == BlockID::TrappedChest) {
            set.boxes[0] = GetBlockShapeAt(world, pos, state);
            set.count    = 1;
        }
        return set;
    }

    const BlockRegistry::BlockShape& BlockRegistry::GetBlockShape(BlockID id) {
        return GetBlockShape(BlockStates::Default(id));
    }

    BlockRegistry::BlockShapeSet BlockRegistry::GetBlockShapeSet(BlockState state) {
        const BlockID id = state.Block();
        BlockStateIndex stateIndex = state.Index();
        const size_t idx = static_cast<size_t>(id);
        BlockShapeSet set;

        // Everything with a single-box shape IS that box, so this API is a
        // drop-in for GetBlockShape at every call site. Built on the spot from
        // the already-cached shape rather than memoised again: a second cache
        // would have to mirror GetBlockShape's "don't cache before the model
        // JSONs load" rule, and getting that wrong freezes a placeholder full
        // cube onto every partial block for the rest of the run.
        const bool multiBox = idx < Size && HasMultiBoxShape(id);
        if (!multiBox) {
            set.boxes[0] = GetBlockShape(state);
            set.count    = 1;
            return set;
        }

        // The multi-box families are memoised. Their box sets come from
        // string-keyed property reads, which is far too much work for a
        // per-voxel query in the collision sweep.
        if (stateIndex >= StateIds().countAt(idx)) stateIndex = 0;
        const uint32_t slot = StateIds().baseAt(idx) + stateIndex;

        StateShapeSetCache& cache = StateShapeSets();
        if (cache.computed[slot].load(std::memory_order_acquire)) {
            return cache.sets[slot];
        }
        set = MultiBoxShape(state, /*collision=*/false);
        cache.sets[slot] = set;
        cache.computed[slot].store(true, std::memory_order_release);
        return set;
    }

    BlockRegistry::BlockShapeSet BlockRegistry::GetBlockCollisionShapeSet(BlockState state) {
        const BlockID id = state.Block();
        // Three families separate the two: fences and walls are taller to walk
        // into than to look at, and an open gate has no collision shape at all.
        // Everything else inherits BlockBehaviour.getCollisionShape's default
        // of "the outline shape".
        if (static_cast<size_t>(id) < Size &&
            (IsCrossCollisionBlock(id) || IsWallBlock(id) || IsFenceGateBlock(id))) {
            return MultiBoxShape(state, /*collision=*/true);
        }
        return GetBlockShapeSet(state);
    }

    // Game::BlockStateIds used to live here — Pack/Unpack/Count/Bits, the shim
    // that let the storage and packet layers keep speaking (BlockID, index)
    // while they were converted. Everything speaks BlockState now, so the
    // shim, its BlockStateRef pair struct and its header are all gone;
    // BlockStates::FromIndex is the one remaining boundary conversion.

    const BlockRegistry::BlockShape& BlockRegistry::GetBlockShape(BlockState state) {
        const BlockID id = state.Block();
        BlockStateIndex stateIndex = state.Index();
        const size_t idx = static_cast<size_t>(id);
        if (idx >= Size) {
            static const BlockShape kFull;
            return kFull;
        }

        StateShapeCache& cache = StateShapes();
        // An out-of-range state index means a save or a peer described a state
        // this build doesn't model. Fall back to the default state rather than
        // indexing past the block's slice into the next block's shapes.
        // BlockStateIds::Pack applies the same clamp for the same reason.
        if (stateIndex >= StateIds().countAt(idx)) stateIndex = 0;

        const uint32_t slot = StateIds().baseAt(idx) + stateIndex;
        BlockShape* const shapes = cache.shapes.data();
        std::atomic<bool>* const computed = cache.computed.get();

        // Fast-path: a slab ALWAYS resolves from its `type` state, regardless of
        // whether the JSON model has loaded yet. Without this the mesher's
        // per-thread EnsureBlockPropsCache can populate the cache with the
        // default full-cube shape during the first mesh build (before
        // BlockModelRegistry::LoadModels has run, or before the worker thread
        // has imported its tables), permanently mis-classifying slabs as full
        // cubes — which made the player fall right through every top slab even
        // though the model JSONs rendered correctly.
        //
        // This now covers all three halves rather than just the top. Before the
        // collapse the bottom happened to be safe because its model name
        // matched a sentinel resolved at vanilla-init time; that was luck, and
        // it does not survive the halves sharing one BlockID and one cache
        // slot per state.
        if (const SlabType half = SlabTypeOf(state); half != SlabType::NotSlab) {
            static const BlockShape kTop =
                BlockShape{ glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(1.0f) };
            static const BlockShape kBottom =
                BlockShape{ glm::vec3(0.0f), glm::vec3(1.0f, 0.5f, 1.0f) };
            static const BlockShape kFullCube =
                BlockShape{ glm::vec3(0.0f), glm::vec3(1.0f) };
            shapes[slot] = (half == SlabType::Top)    ? kTop
                         : (half == SlabType::Bottom) ? kBottom
                                                      : kFullCube;
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

        // ── Stairs ──────────────────────────────────────────────────────────
        //
        // MC StairBlock.getShape is a union of two or three boxes, so what
        // belongs here is its BOUNDS — `getShape().bounds()`, which for a
        // bottom-half stair really is the whole cell. Callers that need the
        // union (collision, the raycast, the occlusion test) ask
        // GetBlockShapeSet instead; the ones that stay on this overload are
        // asking a question the bounds answer correctly.
        //
        // Computed rather than model-derived because the model union would get
        // the same answer by accident and then be wrong the moment the shape
        // and the model disagree — which they do for the top half, where the
        // shape is an INVERT_Y of the bottom one but the model is an x=180
        // rotation with a compensating y turn.
        // Same for every other family whose shape is a union — fences, panes,
        // walls, gates. The bounds are what a caller asking for one AABB wants.
        if (HasMultiBoxShape(id)) {
            const BlockShapeSet set = MultiBoxShape(BlockStates::FromIndex(id, stateIndex),
                                                    /*collision=*/false);
            glm::vec3 mn(std::numeric_limits<float>::infinity());
            glm::vec3 mx(-std::numeric_limits<float>::infinity());
            for (const BlockShape& b : set) {
                mn = glm::min(mn, b.min);
                mx = glm::max(mx, b.max);
            }
            // An open fence gate has no shape at all for collision, but it is
            // still drawn and still selectable, so the OUTLINE set is never
            // empty and this loop always has something to fold.
            shapes[slot] = BlockShape{mn, mx};
            computed[slot].store(true, std::memory_order_release);
            return shapes[slot];
        }

        // ── Crops: one shape per age ────────────────────────────────────────
        //
        // gen_block_shapes.py deliberately skips blocks whose getShape depends
        // on state (see CLAUDE.md), so these have to be written out. They are
        // the same closed-form `Block.column(width, 0, height)` MC uses, in
        // pixels /16 — a crop's hitbox grows with the plant even though the
        // MODEL is the same full-cell cross at every stage. Without this a
        // freshly planted seed would present a full-height selection box.
        //
        // Runs after the s_hasMcShape check would have, and before the
        // model-derived union, because the union is exactly what gets this
        // wrong.
        if (AttachedStemShape(id, stateIndex, shapes[slot])) {
            computed[slot].store(true, std::memory_order_release);
            return shapes[slot];
        }

        if (FaceAttachedShapeFor(id, stateIndex, shapes[slot])) {
            computed[slot].store(true, std::memory_order_release);
            return shapes[slot];
        }

        if (const CropShapeRule* rule = CropShapeRuleFor(id)) {
            const int age = rule->readsAge
                ? AgeFromState(id, stateIndex)
                : 0;
            const float heightPx = static_cast<float>(rule->baseHeightPx +
                                                      age * rule->heightStepPx);
            const float halfW = rule->widthPx * 0.5f / 16.0f;
            shapes[slot] = BlockShape{
                glm::vec3(0.5f - halfW, 0.0f, 0.5f - halfW),
                glm::vec3(0.5f + halfW, heightPx / 16.0f, 0.5f + halfW)
            };
            computed[slot].store(true, std::memory_order_release);
            return shapes[slot];
        }

        // ── MC's own shape, whenever we managed to extract it ──────────────
        // Checked BEFORE the model-derived union below, because the model is
        // simply the wrong source for a shape: MC keeps BlockBehaviour.getShape
        // independent of the rendered geometry, and for the whole cross-model
        // family (saplings, flowers, mushrooms, grass) the model spans the full
        // cell while the real shape is a small centred column. The union gave
        // those blocks a 1x1x1 hitbox.
        //
        // Safe to apply to every state: the generator only emits blocks whose
        // MC shape is state-independent, so no per-state block reaches here
        // with a single cached answer. Blocks it could not resolve fall through
        // and keep the model-derived box, which stays correct for the things
        // that motivated it (slabs, stairs, rotated clumps).
        if (s_hasMcShape[idx]) {
            shapes[slot] = s_mcShape[idx];
            computed[slot].store(true, std::memory_order_release);
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
        const BlockModel& model = GetBlockModel(BlockStates::FromIndex(id, stateIndex));
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
