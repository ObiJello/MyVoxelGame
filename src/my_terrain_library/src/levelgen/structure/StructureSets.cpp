#include "levelgen/structure/StructureSet.h"

#include "external/json.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>

// Reference: net/minecraft/world/level/levelgen/structure/StructureSet.java,
// StructurePlacement codecs, and RegistryDataLoader (alphabetical Identifier
// order). Data: data/minecraft/worldgen/structure_set/*.json (20),
// data/minecraft/worldgen/structure/*.json (34),
// data/minecraft/tags/worldgen/biome/**.json.

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Same discovery contract as the block-tag loader (BlockPredicate.cpp):
// MC_DATA_ROOT override, else walk up from CWD looking for data/.
fs::path findDataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) {
        fs::path candidate(env);
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }
        throw std::runtime_error(std::string("MC_DATA_ROOT is set but is not a directory: ") + env);
    }
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error(
        "Data root not found: no 'data/' directory above the current working "
        "directory and MC_DATA_ROOT is unset (needed for worldgen/structure_set)");
}

std::string normalizeId(const std::string& id, const std::string& defaultNamespace = "minecraft") {
    return id.find(':') != std::string::npos ? id : defaultNamespace + ":" + id;
}

// Split "minecraft:foo/bar" -> ("minecraft", "foo/bar").
std::pair<std::string, std::string> splitId(const std::string& id) {
    size_t colon = id.find(':');
    if (colon == std::string::npos) return {"minecraft", id};
    return {id.substr(0, colon), id.substr(colon + 1)};
}

json loadJsonFile(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open " + path.string());
    }
    json parsed;
    input >> parsed;
    return parsed;
}

FrequencyReductionMethod parseFrequencyReduction(const std::string& name) {
    if (name == "default") return FrequencyReductionMethod::DEFAULT;
    if (name == "legacy_type_1") return FrequencyReductionMethod::LEGACY_TYPE_1;
    if (name == "legacy_type_2") return FrequencyReductionMethod::LEGACY_TYPE_2;
    if (name == "legacy_type_3") return FrequencyReductionMethod::LEGACY_TYPE_3;
    throw std::runtime_error("Unknown frequency_reduction_method: " + name);
}

RandomSpreadType parseSpreadType(const std::string& name) {
    if (name == "linear") return RandomSpreadType::LINEAR;
    if (name == "triangular") return RandomSpreadType::TRIANGULAR;
    throw std::runtime_error("Unknown spread_type: " + name);
}

std::unique_ptr<StructurePlacement> parsePlacement(const json& p, const std::string& setName) {
    // Common placement fields (StructurePlacement.placementCodec).
    int32_t lox = 0, loy = 0, loz = 0;
    if (p.contains("locate_offset")) {
        const auto& lo = p["locate_offset"];
        lox = lo[0].get<int32_t>(); loy = lo[1].get<int32_t>(); loz = lo[2].get<int32_t>();
    }
    FrequencyReductionMethod method = parseFrequencyReduction(
        p.value("frequency_reduction_method", std::string("default")));
    float frequency = p.value("frequency", 1.0f);
    int32_t salt = p.at("salt").get<int32_t>();
    std::optional<ExclusionZone> exclusion;
    if (p.contains("exclusion_zone")) {
        const auto& ez = p["exclusion_zone"];
        exclusion = ExclusionZone{normalizeId(ez.at("other_set").get<std::string>()),
                                  ez.at("chunk_count").get<int32_t>()};
    }

    std::string type = normalizeId(p.at("type").get<std::string>());
    if (type == "minecraft:random_spread") {
        int32_t spacing = p.at("spacing").get<int32_t>();
        int32_t separation = p.at("separation").get<int32_t>();
        RandomSpreadType spreadType = parseSpreadType(p.value("spread_type", std::string("linear")));
        if (spacing <= separation) {
            throw std::runtime_error("Spacing has to be larger than separation in " + setName);
        }
        return std::make_unique<RandomSpreadStructurePlacement>(
            lox, loy, loz, method, frequency, salt, std::move(exclusion),
            spacing, separation, spreadType);
    }
    if (type == "minecraft:concentric_rings") {
        return std::make_unique<ConcentricRingsStructurePlacement>(
            lox, loy, loz, method, frequency, salt, std::move(exclusion),
            p.at("distance").get<int32_t>(), p.at("spread").get<int32_t>(),
            p.at("count").get<int32_t>(), p.at("preferred_biomes").get<std::string>());
    }
    throw std::runtime_error("Unknown structure placement type '" + type + "' in " + setName);
}

class Registry {
public:
    static Registry& instance() {
        static Registry s_instance;
        return s_instance;
    }

    std::vector<const StructureSet*> sets;
    std::vector<const StructureInfo*> structures;
    std::map<std::string, std::unique_ptr<StructureSet>> setsByName;      // sorted = registry order
    std::map<std::string, std::unique_ptr<StructureInfo>> structuresByName;

private:
    Registry() {
        fs::path root = findDataRoot();

        // Structures first (sets reference them). std::map keys sort bytewise,
        // matching Java's Identifier ordering for a single namespace.
        fs::path structureDir = root / "minecraft" / "worldgen" / "structure";
        for (const auto& entry : fs::directory_iterator(structureDir)) {
            if (entry.path().extension() != ".json") continue;
            json parsed = loadJsonFile(entry.path());
            auto info = std::make_unique<StructureInfo>();
            info->name = "minecraft:" + entry.path().stem().string();
            info->type = normalizeId(parsed.at("type").get<std::string>());
            info->biomesTag = parsed.at("biomes").get<std::string>();
            info->step = parsed.at("step").get<std::string>();
            info->terrainAdaptation = parsed.value("terrain_adaptation", std::string("none"));
            info->mineshaftType = parsed.value("mineshaft_type", std::string());
            if (info->type == "minecraft:jigsaw") {
                info->jigsawStartPool = normalizeId(parsed.at("start_pool").get<std::string>());
                if (parsed.contains("start_jigsaw_name")) {
                    info->jigsawStartJigsawName = normalizeId(parsed["start_jigsaw_name"].get<std::string>());
                }
                if (parsed.contains("project_start_to_heightmap")) {
                    info->jigsawProjectToHeightmap = parsed["project_start_to_heightmap"].get<std::string>();
                }
                const auto& sh = parsed.at("start_height");
                if (sh.contains("absolute")) {
                    info->jigsawStartHeightAbsolute = sh["absolute"].get<int>();
                } else if (sh.value("type", std::string()) == "minecraft:uniform") {
                    info->jigsawStartHeightUniform = true;
                    info->jigsawStartHeightMin = sh.at("min_inclusive").at("absolute").get<int>();
                    info->jigsawStartHeightMax = sh.at("max_inclusive").at("absolute").get<int>();
                } else {
                    throw std::runtime_error("Unsupported start_height for " + info->name);
                }
                info->jigsawMaxDepth = parsed.at("size").get<int>();
                info->jigsawExpansionHack = parsed.value("use_expansion_hack", false);
                info->jigsawLiquidSettings =
                    parsed.value("liquid_settings", std::string("apply_waterlogging"));
                if (parsed.contains("max_distance_from_center")) {
                    const auto& md = parsed["max_distance_from_center"];
                    if (md.is_number()) {
                        info->jigsawMaxDistanceH = md.get<int>();
                        info->jigsawMaxDistanceV = md.get<int>();
                    } else {
                        info->jigsawMaxDistanceH = md.at("horizontal").get<int>();
                        info->jigsawMaxDistanceV = md.value("vertical", 384);  // DimensionType.Y_SIZE
                    }
                }
                if (parsed.contains("dimension_padding")) {
                    const auto& dp = parsed["dimension_padding"];
                    if (dp.is_number()) {
                        info->jigsawPaddingBottom = dp.get<int>();
                        info->jigsawPaddingTop = dp.get<int>();
                    } else {
                        info->jigsawPaddingBottom = dp.value("bottom", 0);
                        info->jigsawPaddingTop = dp.value("top", 0);
                    }
                }
                info->jigsawHasAliases = parsed.contains("pool_aliases");
                if (info->jigsawHasAliases) {
                    std::function<StructureInfo::PoolAlias(const json&)> parseAlias =
                        [&](const json& a) -> StructureInfo::PoolAlias {
                        StructureInfo::PoolAlias binding;
                        binding.type = normalizeId(a.at("type").get<std::string>());
                        if (binding.type == "minecraft:direct") {
                            binding.alias = normalizeId(a.at("alias").get<std::string>());
                            binding.target = normalizeId(a.at("target").get<std::string>());
                        } else if (binding.type == "minecraft:random") {
                            binding.alias = normalizeId(a.at("alias").get<std::string>());
                            for (const auto& t : a.at("targets")) {
                                binding.targets.emplace_back(
                                    normalizeId(t.at("data").get<std::string>()),
                                    t.value("weight", 1));
                            }
                        } else if (binding.type == "minecraft:random_group") {
                            for (const auto& g : a.at("groups")) {
                                std::vector<StructureInfo::PoolAlias> group;
                                for (const auto& sub : g.at("data")) {
                                    group.push_back(parseAlias(sub));
                                }
                                binding.groups.emplace_back(std::move(group), g.value("weight", 1));
                            }
                        } else {
                            throw std::runtime_error("Unknown pool alias type: " + binding.type);
                        }
                        return binding;
                    };
                    for (const auto& a : parsed["pool_aliases"]) {
                        info->jigsawAliases.push_back(parseAlias(a));
                    }
                }
            }
            if (parsed.contains("setups")) {
                for (const auto& s : parsed["setups"]) {
                    RuinedPortalSetup setup;
                    setup.placement = s.at("placement").get<std::string>();
                    setup.airPocketProbability = s.value("air_pocket_probability", 0.0f);
                    setup.mossiness = s.value("mossiness", 0.0f);
                    setup.overgrown = s.value("overgrown", false);
                    setup.vines = s.value("vines", false);
                    setup.canBeCold = s.value("can_be_cold", false);
                    setup.replaceWithBlackstone = s.value("replace_with_blackstone", false);
                    setup.weight = s.value("weight", 1.0f);
                    info->portalSetups.push_back(std::move(setup));
                }
            }
            structuresByName.emplace(info->name, std::move(info));
        }
        for (const auto& [name, info] : structuresByName) {
            structures.push_back(info.get());
        }

        fs::path setDir = root / "minecraft" / "worldgen" / "structure_set";
        for (const auto& entry : fs::directory_iterator(setDir)) {
            if (entry.path().extension() != ".json") continue;
            json parsed = loadJsonFile(entry.path());
            auto set = std::make_unique<StructureSet>();
            set->name = "minecraft:" + entry.path().stem().string();
            set->placement = parsePlacement(parsed.at("placement"), set->name);
            for (const auto& structureEntry : parsed.at("structures")) {
                std::string structureName = normalizeId(structureEntry.at("structure").get<std::string>());
                auto it = structuresByName.find(structureName);
                if (it == structuresByName.end()) {
                    throw std::runtime_error("structure_set " + set->name +
                                             " references unknown structure " + structureName);
                }
                set->structures.push_back({it->second.get(), structureEntry.value("weight", 1)});
            }
            setsByName.emplace(set->name, std::move(set));
        }
        for (const auto& [name, set] : setsByName) {
            sets.push_back(set.get());
        }
    }
};

// Biome tag registry: data/<ns>/tags/worldgen/biome/<path>.json with nested
// '#' expansion. Membership-only (no ordered variant needed).
class BiomeTagRegistry {
public:
    static BiomeTagRegistry& instance() {
        static BiomeTagRegistry s_instance;
        return s_instance;
    }

    const std::unordered_set<std::string>& resolve(const std::string& rawTag) {
        std::string tag = rawTag;
        if (!tag.empty() && tag[0] == '#') tag = tag.substr(1);
        tag = normalizeId(tag);

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cache.find(tag);
        if (it != m_cache.end()) return it->second;
        auto inserted = m_cache.emplace(tag, std::unordered_set<std::string>{});
        loadLocked(tag, inserted.first->second);
        return inserted.first->second;
    }

private:
    BiomeTagRegistry() : m_root(findDataRoot()) {}

    void loadLocked(const std::string& tag, std::unordered_set<std::string>& out) {
        auto [ns, path] = splitId(tag);
        fs::path tagPath = m_root / ns / "tags" / "worldgen" / "biome" / (path + ".json");
        if (!fs::exists(tagPath)) {
            // A silently-empty tag would silently drop structure sets - the
            // same invisible-parity-hole failure mode as block tags. Fail loud.
            throw std::runtime_error("Biome tag file missing: " + tagPath.string());
        }
        json parsed = loadJsonFile(tagPath);
        if (!parsed.contains("values") || !parsed["values"].is_array()) return;
        for (const auto& value : parsed["values"]) {
            std::string entry = value.is_string() ? value.get<std::string>()
                                                  : value.at("id").get<std::string>();
            if (!entry.empty() && entry[0] == '#') {
                auto [entryNs, entryPath] = splitId(normalizeId(entry.substr(1), ns));
                loadLocked(entryNs + ":" + entryPath, out);
            } else {
                out.insert(normalizeId(entry, ns));
            }
        }
    }

    fs::path m_root;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_cache;
};

} // namespace

namespace StructureSets {

const std::vector<const StructureSet*>& all() {
    return Registry::instance().sets;
}

const std::vector<const StructureInfo*>& allStructures() {
    return Registry::instance().structures;
}

const StructureSet& byName(const std::string& name) {
    auto& reg = Registry::instance();
    auto it = reg.setsByName.find(name);
    if (it == reg.setsByName.end()) {
        throw std::runtime_error("Unknown structure set: " + name);
    }
    return *it->second;
}

const StructureInfo& structureByName(const std::string& name) {
    auto& reg = Registry::instance();
    auto it = reg.structuresByName.find(name);
    if (it == reg.structuresByName.end()) {
        throw std::runtime_error("Unknown structure: " + name);
    }
    return *it->second;
}

} // namespace StructureSets

namespace BiomeTags {

const std::unordered_set<std::string>& resolve(const std::string& tag) {
    return BiomeTagRegistry::instance().resolve(tag);
}

} // namespace BiomeTags

} // namespace structure
} // namespace levelgen
} // namespace minecraft
