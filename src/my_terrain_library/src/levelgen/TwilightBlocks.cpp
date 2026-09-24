#include "levelgen/TwilightBlocks.h"

#include "world/level/block/Blocks.h"
#include "world/level/block/Block.h"
#include "world/level/block/state/BlockState.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Twilight Forest block-name resolution — see TwilightBlocks.h.
//
// RENAMES are the same block under another registry id (not stand-ins):
//   TF mangrove_{log,wood,leaves,sapling,planks,...} -> tf_mangrove_*
//   (the vanilla mangrove set owns the plain names).
// STAND-INS are the nearest block the engine has for a TF block that is not
// registered yet (entries are dropped from the table once Blocks.cpp registers
// the block: deadrock, trollsteinn, towerwood, castle bricks, thorns,
// huge_water_lily, cinder wood, the metal blocks, uncrafting table). Every lookup tries the real slug FIRST, so a stand-in only
// applies while its block is missing, and it is logged once when it does.

namespace minecraft {
namespace levelgen {
namespace twilight_blocks {

namespace {

using world::level::block::Blocks;
using world::level::block::Block;

std::string stripNamespace(const std::string& name) {
    const size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

bool registered(const std::string& id) {
    return Blocks::getBlock(id) != nullptr;
}

// TF name (no namespace) -> engine slug of the SAME block.
const std::unordered_map<std::string, std::string>& renames() {
    static const std::unordered_map<std::string, std::string> table = {
        {"mangrove_log", "tf_mangrove_log"},
        {"mangrove_wood", "tf_mangrove_wood"},
        {"mangrove_leaves", "tf_mangrove_leaves"},
        {"mangrove_sapling", "tf_mangrove_sapling"},
        {"mangrove_planks", "tf_mangrove_planks"},
        {"stripped_mangrove_log", "stripped_tf_mangrove_log"},
        {"stripped_mangrove_wood", "stripped_tf_mangrove_wood"},
        {"mangrove_stairs", "tf_mangrove_stairs"},
        {"mangrove_slab", "tf_mangrove_slab"},
        {"mangrove_fence", "tf_mangrove_fence"},
        {"mangrove_fence_gate", "tf_mangrove_fence_gate"},
        {"mangrove_door", "tf_mangrove_door"},
        {"mangrove_trapdoor", "tf_mangrove_trapdoor"},
        {"mangrove_button", "tf_mangrove_button"},
        {"mangrove_pressure_plate", "tf_mangrove_pressure_plate"},
        {"mangrove_root", "tf_mangrove_root"},
        {"mangrove_banister", "tf_mangrove_banister"},
        // TF's smoker vent; vanilla owns minecraft:smoker (the furnace).
        {"smoker", "tf_smoker"},
    };
    return table;
}

// Vanilla ids the mod's NBT / Java still use that 26.x renamed.
const std::unordered_map<std::string, std::string>& vanillaRenames() {
    static const std::unordered_map<std::string, std::string> table = {
        {"minecraft:chain", "minecraft:iron_chain"},
        {"minecraft:grass", "minecraft:short_grass"},
    };
    return table;
}

// TF name (no namespace) -> ordered stand-ins (engine ids).
const std::unordered_map<std::string, std::vector<std::string>>& standIns() {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        // ---- wood families ("wood" = bark on all faces)
        {"twilight_oak_wood", {"minecraft:twilight_oak_log"}},
        {"canopy_wood", {"minecraft:canopy_log"}},
        {"dark_wood", {"minecraft:dark_log"}},
        {"tf_mangrove_wood", {"minecraft:tf_mangrove_log"}},
        {"tf_mangrove_root", {"minecraft:mangrove_roots"}},
        {"stripped_tf_mangrove_wood", {"minecraft:stripped_tf_mangrove_log"}},
        {"stripped_twilight_oak_wood", {"minecraft:stripped_twilight_oak_log"}},
        {"stripped_canopy_wood", {"minecraft:stripped_canopy_log"}},
        {"stripped_dark_wood", {"minecraft:stripped_dark_log"}},
        {"rainbow_oak_leaves", {"minecraft:twilight_oak_leaves"}},
        {"rainbow_oak_sapling", {"minecraft:twilight_oak_sapling"}},
        {"time_log", {"minecraft:twilight_oak_log"}},
        {"time_wood", {"minecraft:twilight_oak_log"}},
        {"time_leaves", {"minecraft:twilight_oak_leaves"}},
        {"transformation_log", {"minecraft:twilight_oak_log"}},
        {"transformation_wood", {"minecraft:twilight_oak_log"}},
        {"transformation_leaves", {"minecraft:twilight_oak_leaves"}},
        {"mining_log", {"minecraft:twilight_oak_log"}},
        {"mining_wood", {"minecraft:twilight_oak_log"}},
        {"mining_leaves", {"minecraft:twilight_oak_leaves"}},
        {"sorting_log", {"minecraft:twilight_oak_log"}},
        {"sorting_wood", {"minecraft:twilight_oak_log"}},
        {"sorting_leaves", {"minecraft:twilight_oak_leaves"}},
        {"time_log_core", {"minecraft:twilight_oak_log"}},
        {"transformation_log_core", {"minecraft:twilight_oak_log"}},
        {"mining_log_core", {"minecraft:twilight_oak_log"}},
        {"sorting_log_core", {"minecraft:twilight_oak_log"}},
        {"beanstalk_leaves", {"minecraft:twilight_oak_leaves"}},
        {"giant_log", {"minecraft:oak_log"}},
        {"giant_leaves", {"minecraft:oak_leaves"}},
        // Planks-family pieces fall back to the oak set of the same shape.
        {"twilight_oak_stairs", {"minecraft:oak_stairs"}},
        {"twilight_oak_slab", {"minecraft:oak_slab"}},
        {"twilight_oak_fence", {"minecraft:oak_fence"}},
        {"twilight_oak_fence_gate", {"minecraft:oak_fence_gate"}},
        {"twilight_oak_door", {"minecraft:oak_door"}},
        {"twilight_oak_trapdoor", {"minecraft:oak_trapdoor"}},
        {"canopy_stairs", {"minecraft:spruce_stairs"}},
        {"canopy_slab", {"minecraft:spruce_slab"}},
        {"canopy_fence", {"minecraft:spruce_fence"}},
        {"canopy_fence_gate", {"minecraft:spruce_fence_gate"}},
        {"canopy_door", {"minecraft:spruce_door"}},
        {"canopy_trapdoor", {"minecraft:spruce_trapdoor"}},
        {"canopy_bookshelf", {"minecraft:bookshelf"}},
        {"dark_stairs", {"minecraft:dark_oak_stairs"}},
        {"dark_slab", {"minecraft:dark_oak_slab"}},
        {"dark_fence", {"minecraft:dark_oak_fence"}},
        {"dark_fence_gate", {"minecraft:dark_oak_fence_gate"}},
        {"tf_mangrove_stairs", {"minecraft:mangrove_stairs"}},
        {"tf_mangrove_slab", {"minecraft:mangrove_slab"}},
        {"tf_mangrove_fence", {"minecraft:mangrove_fence"}},
        // Banisters (railing on a slab/stair edge): the wood's fence.
        {"oak_banister", {"minecraft:oak_fence"}},
        {"spruce_banister", {"minecraft:spruce_fence"}},
        {"birch_banister", {"minecraft:birch_fence"}},
        {"jungle_banister", {"minecraft:jungle_fence"}},
        {"acacia_banister", {"minecraft:acacia_fence"}},
        {"dark_oak_banister", {"minecraft:dark_oak_fence"}},
        {"twilight_oak_banister", {"minecraft:twilight_oak_fence", "minecraft:oak_fence"}},
        {"canopy_banister", {"minecraft:canopy_fence", "minecraft:spruce_fence"}},
        {"tf_mangrove_banister", {"minecraft:tf_mangrove_fence", "minecraft:mangrove_fence"}},
        {"dark_banister", {"minecraft:dark_fence", "minecraft:dark_oak_fence"}},
        // Hollow logs: the solid log of the same wood.
        {"hollow_oak_log_horizontal", {"minecraft:oak_log"}},
        {"hollow_spruce_log_horizontal", {"minecraft:spruce_log"}},
        {"hollow_birch_log_horizontal", {"minecraft:birch_log"}},
        {"hollow_twilight_oak_log_horizontal", {"minecraft:twilight_oak_log"}},
        {"hollow_canopy_log_horizontal", {"minecraft:canopy_log"}},
        {"hollow_tf_mangrove_log_horizontal", {"minecraft:tf_mangrove_log"}},
        {"hollow_mangrove_log_horizontal", {"minecraft:tf_mangrove_log"}},
        {"hollow_oak_log_vertical", {"minecraft:oak_log"}},
        {"hollow_spruce_log_vertical", {"minecraft:spruce_log"}},
        {"hollow_birch_log_vertical", {"minecraft:birch_log"}},
        {"hollow_twilight_oak_log_vertical", {"minecraft:twilight_oak_log"}},
        {"hollow_canopy_log_vertical", {"minecraft:canopy_log"}},
        {"hollow_oak_log_climbable", {"minecraft:oak_log"}},
        {"hollow_spruce_log_climbable", {"minecraft:spruce_log"}},
        {"hollow_twilight_oak_log_climbable", {"minecraft:twilight_oak_log"}},
        // ---- critters, jars, lights, rope
        {"firefly_jar", {"minecraft:lantern"}},
        {"cicada_jar", {"minecraft:lantern"}},
        {"moonworm_jar", {"minecraft:lantern"}},
        {"firefly_spawner", {"minecraft:lantern"}},
        {"rope", {"minecraft:iron_chain"}},
        {"candelabra", {"minecraft:candle"}},
        {"iron_ladder", {"minecraft:ladder"}},
        {"wither_skeleton_skull_candle", {"minecraft:wither_skeleton_skull"}},
        {"skeleton_skull_candle", {"minecraft:skeleton_skull"}},
        {"zombie_skull_candle", {"minecraft:zombie_head"}},
        {"creeper_skull_candle", {"minecraft:creeper_head"}},
        {"player_skull_candle", {"minecraft:player_head"}},
        // ---- plants
        {"raspberry_bush", {"minecraft:sweet_berry_bush"}},
        {"blueberry_bush", {"minecraft:sweet_berry_bush"}},
        {"blackberry_bush", {"minecraft:sweet_berry_bush"}},
        {"maloberry_bush", {"minecraft:sweet_berry_bush"}},
        {"iron_oreberry_bush", {"minecraft:sweet_berry_bush"}},
        {"gold_oreberry_bush", {"minecraft:sweet_berry_bush"}},
        {"copper_oreberry_bush", {"minecraft:sweet_berry_bush"}},
        {"essence_oreberry_bush", {"minecraft:sweet_berry_bush"}},
        {"huge_lily_pad", {"minecraft:lily_pad"}},
        {"thorns", {"minecraft:dark_oak_log"}},
        {"thorn_rose", {"minecraft:rose_bush", "minecraft:poppy"}},
        {"thorn_leaves", {"minecraft:oak_leaves"}},
        {"root_strand", {"minecraft:hanging_roots"}},
        {"trollvidr", {"minecraft:glow_lichen"}},
        {"unripe_trollber", {"minecraft:glow_lichen"}},
        {"trollber", {"minecraft:glow_lichen"}},
        {"huge_mushgloom", {"minecraft:brown_mushroom_block"}},
        {"huge_mushgloom_stem", {"minecraft:mushroom_stem"}},
        {"uberous_soil", {"minecraft:farmland", "minecraft:dirt"}},
        {"arctic_fur_block", {"minecraft:white_wool"}},
        // ---- fire swamp
        {"fire_jet", {"minecraft:magma_block"}},
        {"encased_fire_jet", {"minecraft:magma_block"}},
        {"tf_smoker", {"minecraft:magma_block"}},
        {"encased_smoker", {"minecraft:magma_block"}},
        // ---- stone families (towerwood, castle, deadrock, trollsteinn, maze, naga)
        {"reappearing_block", {"minecraft:dark_oak_planks"}},
        {"vanishing_block", {"minecraft:dark_oak_planks"}},
        {"nagastone", {"minecraft:stone_bricks"}},
        {"etched_nagastone", {"minecraft:chiseled_stone_bricks"}},
        {"cracked_etched_nagastone", {"minecraft:chiseled_stone_bricks"}},
        {"mossy_etched_nagastone", {"minecraft:chiseled_stone_bricks"}},
        {"nagastone_pillar", {"minecraft:stone_bricks"}},
        {"cracked_nagastone_pillar", {"minecraft:cracked_stone_bricks"}},
        {"mossy_nagastone_pillar", {"minecraft:mossy_stone_bricks"}},
        {"nagastone_head", {"minecraft:chiseled_stone_bricks"}},
        {"nagastone_stairs_left", {"minecraft:stone_brick_stairs"}},
        {"nagastone_stairs_right", {"minecraft:stone_brick_stairs"}},
        {"cracked_nagastone_stairs_left", {"minecraft:stone_brick_stairs"}},
        {"cracked_nagastone_stairs_right", {"minecraft:stone_brick_stairs"}},
        {"mossy_nagastone_stairs_left", {"minecraft:mossy_stone_brick_stairs", "minecraft:stone_brick_stairs"}},
        {"mossy_nagastone_stairs_right", {"minecraft:mossy_stone_brick_stairs", "minecraft:stone_brick_stairs"}},
        {"spiral_bricks", {"minecraft:stone_bricks"}},
        {"bold_stone_pillar", {"minecraft:quartz_pillar", "minecraft:stone_bricks"}},
        {"twisted_stone_pillar", {"minecraft:stone_brick_wall", "minecraft:stone_bricks"}},
        {"twisted_stone", {"minecraft:stone_bricks"}},
        {"terrorcotta_arcs", {"minecraft:white_terracotta", "minecraft:terracotta"}},
        {"terrorcotta_curves", {"minecraft:white_terracotta", "minecraft:terracotta"}},
        {"terrorcotta_lines", {"minecraft:white_terracotta", "minecraft:terracotta"}},
        {"wrought_iron_fence", {"minecraft:iron_bars"}},
        {"coronation_carpet", {"minecraft:red_carpet"}},
        {"violet_force_field", {"minecraft:purple_stained_glass_pane"}},
        {"underbrick", {"minecraft:bricks"}},
        {"mossy_underbrick", {"minecraft:bricks"}},
        {"cracked_underbrick", {"minecraft:bricks"}},
        {"underbrick_floor", {"minecraft:bricks"}},
        {"stronghold_shield", {"minecraft:polished_blackstone"}},
        {"trophy_pedestal", {"minecraft:chiseled_stone_bricks"}},
        // ---- containers, spawners (boss spawners are markers only)
        {"canopy_chest", {"minecraft:chest"}},
        {"twilight_oak_chest", {"minecraft:chest"}},
        {"dark_chest", {"minecraft:chest"}},
        {"tf_mangrove_chest", {"minecraft:chest"}},
        {"keepsake_casket", {"minecraft:chest"}},
        {"naga_boss_spawner", {"minecraft:spawner"}},
        {"lich_boss_spawner", {"minecraft:spawner"}},
        {"minoshroom_boss_spawner", {"minecraft:spawner"}},
        {"hydra_boss_spawner", {"minecraft:spawner"}},
        {"knight_phantom_boss_spawner", {"minecraft:spawner"}},
        {"ur_ghast_boss_spawner", {"minecraft:spawner"}},
        {"alpha_yeti_boss_spawner", {"minecraft:spawner"}},
        {"snow_queen_boss_spawner", {"minecraft:spawner"}},
        {"sinister_spawner", {"minecraft:spawner"}},
        {"wood_roots_ore", {"minecraft:root"}},
    };
    return table;
}

// Last-resort by shape, for a TF name missing from both lists.
std::string shapeFallback(const std::string& slug) {
    auto endsWith = [&slug](const char* suffix) {
        const std::string s(suffix);
        return slug.size() >= s.size() && slug.compare(slug.size() - s.size(), s.size(), s) == 0;
    };
    if (endsWith("_stairs")) return "minecraft:stone_brick_stairs";
    if (endsWith("_slab")) return "minecraft:stone_brick_slab";
    if (endsWith("_wall")) return "minecraft:stone_brick_wall";
    if (endsWith("_fence") || endsWith("_banister")) return "minecraft:oak_fence";
    if (endsWith("_leaves")) return "minecraft:oak_leaves";
    if (endsWith("_log") || endsWith("_wood")) return "minecraft:oak_log";
    if (endsWith("_planks")) return "minecraft:oak_planks";
    if (endsWith("_sapling")) return "minecraft:oak_sapling";
    if (endsWith("_spawner")) return "minecraft:spawner";
    if (endsWith("_chest")) return "minecraft:chest";
    if (endsWith("_carpet")) return "minecraft:red_carpet";
    if (endsWith("_pane")) return "minecraft:glass_pane";
    if (endsWith("_glass")) return "minecraft:glass";
    if (endsWith("_terracotta")) return "minecraft:terracotta";
    if (endsWith("_concrete")) return "minecraft:white_concrete";
    if (endsWith("_sign")) return "minecraft:oak_sign";
    return "minecraft:stone";
}

struct Resolution {
    std::string id;     // "" = nothing
    bool standIn = false;
};

Resolution computeResolution(const std::string& rawName) {
    std::string name = rawName;
    const bool twilight = name.rfind("twilightforest:", 0) == 0;
    if (!twilight) {
        if (name.find(':') == std::string::npos) name = "minecraft:" + name;
        if (registered(name)) return {name, false};
        auto vr = vanillaRenames().find(name);
        if (vr != vanillaRenames().end() && registered(vr->second)) return {vr->second, false};
        // A "minecraft:" slug may be a TF block written the engine's way.
    }
    std::string slug = stripNamespace(name);
    auto rn = renames().find(slug);
    if (rn != renames().end()) slug = rn->second;
    const std::string own = "minecraft:" + slug;
    if (registered(own)) return {own, false};

    auto si = standIns().find(slug);
    if (si != standIns().end()) {
        for (const std::string& candidate : si->second) {
            if (registered(candidate)) return {candidate, true};
        }
    }
    // Last resort, for TF names and for vanilla ids a TF template uses that
    // this build lacks: a block of the same shape family (logged).
    const std::string fallback = shapeFallback(slug);
    if (registered(fallback)) return {fallback, true};
    if (registered("minecraft:stone")) return {"minecraft:stone", true};
    (void)twilight;
    return {"", false};
}

std::mutex s_mutex;
std::unordered_map<std::string, Resolution> s_cache;

const Resolution& resolve(const std::string& name) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(name);
    if (it != s_cache.end()) return it->second;
    Resolution r = computeResolution(name);
    if (r.id.empty()) {
        fprintf(stderr, "[TwilightBlocks] %s: no block and no stand-in registered\n", name.c_str());
    } else if (r.standIn) {
        fprintf(stderr, "[TwilightBlocks] %s not registered - stand-in %s\n", name.c_str(), r.id.c_str());
    }
    return s_cache.emplace(name, std::move(r)).first->second;
}

} // namespace

std::string resolveName(const std::string& name) {
    return resolve(name).id;
}

bool isStandIn(const std::string& name) {
    return resolve(name).standIn;
}

BlockState* defaultState(const std::string& name) {
    const std::string& id = resolve(name).id;
    return id.empty() ? nullptr : Blocks::getDefaultState(id);
}

BlockState* state(const std::string& name,
                  const std::unordered_map<std::string, std::string>& properties) {
    const std::string& id = resolve(name).id;
    if (id.empty()) return nullptr;
    Block* block = Blocks::getBlock(id);
    if (block == nullptr) return nullptr;
    BlockState* best = block->defaultBlockState();
    if (properties.empty()) return best;

    // Keep only the requested properties the block has, then take the state
    // matching the most of them (exact match on every kept key wins; a value
    // the block does not accept leaves that key at its default).
    const auto defaults = best->getProperties();
    int bestScore = -1;
    for (BlockState* candidate : block->getStateDefinition().getPossibleStates()) {
        const auto have = candidate->getProperties();
        int score = 0;
        bool consistent = true;
        for (const auto& [key, value] : have) {
            auto want = properties.find(key);
            if (want != properties.end() && want->second == value) {
                ++score;
            } else if (want != properties.end()) {
                // Requested value differs: acceptable only when the block
                // cannot take the requested value at all (then its default
                // stands) — penalise states that leave the default.
                auto def = defaults.find(key);
                if (def != defaults.end() && def->second != value) {
                    bool accepts = false;
                    for (BlockState* other : block->getStateDefinition().getPossibleStates()) {
                        auto p = other->getProperties();
                        auto v = p.find(key);
                        if (v != p.end() && v->second == want->second) { accepts = true; break; }
                    }
                    if (accepts) { consistent = false; break; }
                }
            } else {
                // Not requested: must stay at the default value.
                auto def = defaults.find(key);
                if (def != defaults.end() && def->second != value) { consistent = false; break; }
            }
        }
        if (consistent && score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

} // namespace twilight_blocks
} // namespace levelgen
} // namespace minecraft
