#include "levelgen/AetherBlocks.h"

#include "world/level/block/Blocks.h"
#include "world/level/block/Block.h"
#include "world/level/block/state/BlockState.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// The Aether block-name resolution — see AetherBlocks.h.
//
// STAND-INS are the nearest block the engine has for an Aether block that is
// not registered yet. Every lookup tries the real slug FIRST, so a stand-in
// only applies while its block is missing, and it is logged once when it does.
// Dungeon-stone variants (locked_, trapped_, boss_doorway_, treasure_doorway_)
// fall back to their base stone first, then to that stone's stand-in.

namespace minecraft {
namespace levelgen {
namespace aether_blocks {

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

// Aether name (no namespace) -> ordered stand-ins (engine ids).
const std::unordered_map<std::string, std::vector<std::string>>& standIns() {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        // ---- dungeon stone
        {"carved_stone", {"minecraft:stone_bricks"}},
        {"sentry_stone", {"minecraft:chiseled_stone_bricks"}},
        {"angelic_stone", {"minecraft:smooth_quartz", "minecraft:quartz_block"}},
        {"light_angelic_stone", {"minecraft:sea_lantern", "minecraft:glowstone"}},
        {"hellfire_stone", {"minecraft:red_nether_bricks", "minecraft:nether_bricks"}},
        {"light_hellfire_stone", {"minecraft:shroomlight", "minecraft:glowstone"}},
        {"pillar", {"minecraft:quartz_pillar"}},
        {"pillar_top", {"minecraft:quartz_pillar"}},
        // ---- dungeon furniture
        {"treasure_chest", {"minecraft:chest"}},
        {"chest_mimic", {"minecraft:chest"}},
        {"ambrosium_torch", {"minecraft:torch"}},
        {"ambrosium_wall_torch", {"minecraft:wall_torch"}},
        {"aerogel", {"minecraft:light_blue_stained_glass", "minecraft:glass"}},
        {"zanite_block", {"minecraft:amethyst_block"}},
        {"present", {"minecraft:snow"}},
        // ---- holystone building set
        {"holystone_bricks", {"minecraft:stone_bricks"}},
        {"holystone_slab", {"minecraft:cobblestone_slab"}},
        {"holystone_stairs", {"minecraft:cobblestone_stairs"}},
        {"holystone_wall", {"minecraft:cobblestone_wall"}},
        {"mossy_holystone", {"minecraft:mossy_cobblestone"}},
        {"mossy_holystone_slab", {"minecraft:mossy_cobblestone_slab"}},
        {"mossy_holystone_stairs", {"minecraft:mossy_cobblestone_stairs"}},
        {"mossy_holystone_wall", {"minecraft:mossy_cobblestone_wall"}},
        // ---- skyroot building set (the oak set of the same shape)
        {"skyroot_stairs", {"minecraft:oak_stairs"}},
        {"skyroot_slab", {"minecraft:oak_slab"}},
        {"skyroot_fence", {"minecraft:oak_fence"}},
        {"skyroot_fence_gate", {"minecraft:oak_fence_gate"}},
        {"skyroot_door", {"minecraft:oak_door"}},
        {"skyroot_trapdoor", {"minecraft:oak_trapdoor"}},
        {"skyroot_button", {"minecraft:oak_button"}},
        {"skyroot_pressure_plate", {"minecraft:oak_pressure_plate"}},
        {"skyroot_wood", {"minecraft:skyroot_log", "minecraft:oak_wood"}},
        {"skyroot_bookshelf", {"minecraft:bookshelf"}},
        // ---- ground and plants
        {"enchanted_aether_grass_block", {"minecraft:aether_grass_block"}},
        {"crystal_leaves", {"minecraft:skyroot_leaves"}},
        {"crystal_fruit_leaves", {"minecraft:skyroot_leaves"}},
        {"holiday_leaves", {"minecraft:skyroot_leaves"}},
        {"decorated_holiday_leaves", {"minecraft:skyroot_leaves"}},
    };
    return table;
}

// Prefixes of the dungeon-stone variants that fall back to their base stone.
const char* const kVariantPrefixes[] = {
    "locked_", "trapped_", "boss_doorway_", "treasure_doorway_"};

struct Resolution {
    std::string id;         // "" when nothing resolves
    bool standIn = false;
};

std::vector<std::string> candidatesFor(const std::string& slug) {
    std::vector<std::string> out;
    out.push_back("minecraft:" + slug);
    auto it = standIns().find(slug);
    if (it != standIns().end()) {
        out.insert(out.end(), it->second.begin(), it->second.end());
    }
    for (const char* prefix : kVariantPrefixes) {
        const std::string p(prefix);
        if (slug.rfind(p, 0) == 0) {
            for (const std::string& base : candidatesFor(slug.substr(p.size()))) {
                out.push_back(base);
            }
        }
    }
    return out;
}

const Resolution& resolve(const std::string& name) {
    static std::mutex s_mutex;
    static std::unordered_map<std::string, Resolution> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(name);
    if (it != s_cache.end()) return it->second;

    Resolution result;
    const std::string slug = stripNamespace(name);
    if (!isAetherId(name) && registered(name)) {
        result.id = name;   // a vanilla id the registry has
    } else {
        const std::vector<std::string> candidates = candidatesFor(slug);
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (registered(candidates[i])) {
                result.id = candidates[i];
                result.standIn = i > 0;
                break;
            }
        }
        if (result.standIn) {
            std::fprintf(stderr, "[AetherBlocks] %s not registered - stand-in %s\n",
                         name.c_str(), result.id.c_str());
        } else if (result.id.empty()) {
            std::fprintf(stderr, "[AetherBlocks] %s not registered and has no stand-in\n",
                         name.c_str());
        }
    }
    return s_cache.emplace(name, std::move(result)).first->second;
}

} // namespace

bool isAetherId(const std::string& name) {
    return name.rfind("aether:", 0) == 0;
}

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
    BlockState* defaultBlockState = block->defaultBlockState();
    if (properties.empty()) return defaultBlockState;

    const auto& possible = block->getStateDefinition().getPossibleStates();
    // Values each property of the block accepts.
    std::map<std::string, std::set<std::string>> accepted;
    for (BlockState* candidate : possible) {
        for (const auto& [key, value] : candidate->getProperties()) {
            accepted[key].insert(value);
        }
    }
    // Target: the requested value where the block has the key and accepts the
    // value, else the default.
    std::map<std::string, std::string> target;
    for (const auto& [key, value] : defaultBlockState->getProperties()) {
        auto want = properties.find(key);
        auto acc = accepted.find(key);
        if (want != properties.end() && acc != accepted.end() && acc->second.count(want->second)) {
            target[key] = want->second;
        } else {
            target[key] = value;
        }
    }
    for (BlockState* candidate : possible) {
        bool match = true;
        for (const auto& [key, value] : candidate->getProperties()) {
            auto t = target.find(key);
            if (t == target.end() || t->second != value) { match = false; break; }
        }
        if (match) return candidate;
    }
    return defaultBlockState;
}

} // namespace aether_blocks
} // namespace levelgen
} // namespace minecraft
