// File: src/common/world/biome/BiomeAttributes.cpp
#include "common/world/biome/BiomeAttributes.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Game::BiomeAttributes {

    namespace {

        // Same rule as DataTags and the terrain library: MC_DATA_ROOT, else
        // ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        // The constant boolean attributes of one biome ("attributes" values
        // that are JSON booleans; a modifier object or a timeline-driven value
        // is not a constant and is left out).
        using BoolAttributes = std::map<std::string, bool, std::less<>>;

        BoolAttributes LoadBiome(BiomeId biome) {
            BoolAttributes out;
            const std::string_view name = BiomeRegistry::Get(biome).name;
            std::string ns = "minecraft";
            std::string path(name);
            if (const size_t colon = name.find(':'); colon != std::string_view::npos) {
                ns = std::string(name.substr(0, colon));
                path = std::string(name.substr(colon + 1));
            }
            const std::filesystem::path file = DataRoot() / ns / "worldgen" / "biome" / (path + ".json");
            std::ifstream in(file);
            if (!in) return out;
            try {
                nlohmann::json json;
                in >> json;
                const auto it = json.find("attributes");
                if (it == json.end() || !it->is_object()) return out;
                for (const auto& [key, value] : it->items()) {
                    if (value.is_boolean()) out.emplace(key, value.get<bool>());
                }
            } catch (const std::exception&) {
                out.clear();
            }
            return out;
        }

        std::mutex g_mutex;
        std::unordered_map<BiomeId, BoolAttributes> g_cache;

    } // namespace

    std::optional<bool> GetBool(BiomeId biome, std::string_view attribute) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cache.find(biome);
        if (it == g_cache.end()) it = g_cache.emplace(biome, LoadBiome(biome)).first;
        if (const auto found = it->second.find(attribute); found != it->second.end()) return found->second;
        return std::nullopt;
    }

    bool SnowGolemMelts(DimensionId dimension, BiomeId biome) {
        constexpr std::string_view kAttribute = "minecraft:gameplay/snow_golem_melts";
        if (const std::optional<bool> value = GetBool(biome, kAttribute)) return *value;
        // dimension_type/the_nether.json: "minecraft:gameplay/snow_golem_melts":
        // true — the one dimension type that sets it.
        return dimension == DimensionId::Nether;
    }

} // namespace Game::BiomeAttributes
