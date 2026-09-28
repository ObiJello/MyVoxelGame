// File: src/common/world/banner/BannerPatterns.cpp
#include "common/world/banner/BannerPatterns.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

namespace Game::BannerPatterns {

    namespace {

        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        std::filesystem::path PathFor(const std::string& id, const char* folder) {
            const size_t colon = id.find(':');
            return DataRoot() / id.substr(0, colon) / folder / (id.substr(colon + 1) + ".json");
        }

        bool ReadJson(const std::filesystem::path& file, nlohmann::json& out) {
            std::ifstream in(file);
            if (!in) return false;
            try { in >> out; } catch (const std::exception&) { return false; }
            return true;
        }

        std::mutex g_mutex;
        struct Definition_ {
            std::string asset;            // "" = no definition
            std::string translationKey;
        };
        std::unordered_map<std::string, Definition_> g_defs;
        std::unordered_map<std::string, std::vector<std::string>> g_tags;

        // Caller holds g_mutex.
        const Definition_* Definition(const std::string& id) {
            auto it = g_defs.find(id);
            if (it == g_defs.end()) {
                Definition_ def;
                nlohmann::json j;
                if (ReadJson(PathFor(id, "banner_pattern"), j) && j.is_object()) {
                    def.asset = j.value("asset_id", id);
                    if (const size_t colon = def.asset.find(':'); colon != std::string::npos) def.asset = def.asset.substr(colon + 1);
                    def.translationKey = j.value("translation_key", std::string());
                }
                it = g_defs.emplace(id, std::move(def)).first;
            }
            return it->second.asset.empty() ? nullptr : &it->second;
        }

        void ExpandTag(const std::string& tag, std::vector<std::string>& out, std::set<std::string>& visiting) {
            if (!visiting.insert(tag).second) return;
            nlohmann::json j;
            if (!ReadJson(PathFor(tag, "tags/banner_pattern"), j) || !j.is_object() ||
                !j.contains("values") || !j["values"].is_array()) {
                return;
            }
            for (const nlohmann::json& entry : j["values"]) {
                std::string value;
                if (entry.is_string()) value = entry.get<std::string>();
                else if (entry.is_object() && entry.contains("id") && entry["id"].is_string()) value = entry["id"].get<std::string>();
                if (value.empty()) continue;
                if (value[0] == '#') ExpandTag(WithNamespace(value.substr(1)), out, visiting);
                else {
                    const std::string id = WithNamespace(value);
                    bool seen = false;
                    for (const std::string& e : out) seen = seen || e == id;
                    if (!seen && Definition(id)) out.push_back(id);
                }
            }
        }

    } // namespace

    std::vector<std::string> Resolve(const std::string& spec) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (spec.empty()) return {};
        if (spec[0] != '#') {
            const std::string id = WithNamespace(spec);
            return Definition(id) ? std::vector<std::string>{id} : std::vector<std::string>{};
        }
        const std::string tag = WithNamespace(spec.substr(1));
        if (auto it = g_tags.find(tag); it != g_tags.end()) return it->second;
        std::vector<std::string> out;
        std::set<std::string> visiting;
        ExpandTag(tag, out, visiting);
        g_tags[tag] = out;
        return out;
    }

    bool Exists(const std::string& id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return Definition(WithNamespace(id)) != nullptr;
    }

    std::string AssetOf(const std::string& id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::string full = WithNamespace(id);
        if (const Definition_* def = Definition(full)) return def->asset;
        return full.substr(full.find(':') + 1);
    }

    std::string TranslationKeyOf(const std::string& id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::string full = WithNamespace(id);
        if (const Definition_* def = Definition(full); def && !def->translationKey.empty()) return def->translationKey;
        const size_t colon = full.find(':');
        return "block." + full.substr(0, colon) + ".banner." + full.substr(colon + 1);
    }

    std::string ProvidedTagOf(const std::string& itemSlug) {
        constexpr const char kSuffix[] = "_banner_pattern";
        constexpr size_t kLen = sizeof(kSuffix) - 1;
        if (itemSlug.size() <= kLen || itemSlug.compare(itemSlug.size() - kLen, kLen, kSuffix) != 0) return {};
        return "#minecraft:pattern_item/" + itemSlug.substr(0, itemSlug.size() - kLen);
    }

} // namespace Game::BannerPatterns
