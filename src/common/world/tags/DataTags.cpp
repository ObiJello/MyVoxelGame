// File: src/common/world/tags/DataTags.cpp
#include "DataTags.hpp"
#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

namespace Game::DataTags {

    namespace {

        // Same rule as the terrain library and /locate: MC_DATA_ROOT, else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        const char* RegistryDir(Registry r) {
            switch (r) {
                case Registry::Block:      return "block";
                case Registry::Fluid:      return "fluid";
                case Registry::EntityType: return "entity_type";
                case Registry::Item:       return "item";
            }
            return "block";
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        struct Index {
            bool loaded = false;
            // tag name ("minecraft:logs") -> raw entries (ids and "#tags")
            std::unordered_map<std::string, std::vector<std::string>> rawTags;
            // resolved: id -> sorted tag list, as "#ns:path"
            std::unordered_map<std::string, std::vector<std::string>> byId;
            std::vector<std::string> empty;
        };

        Index g_index[4];
        std::mutex g_mutex;

        void ScanRegistry(Index& index, Registry registry) {
            const std::filesystem::path root = DataRoot();
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) return;
            for (const auto& nsEntry : std::filesystem::directory_iterator(root, ec)) {
                if (!nsEntry.is_directory()) continue;
                const std::string ns = nsEntry.path().filename().string();
                const std::filesystem::path tagDir = nsEntry.path() / "tags" / RegistryDir(registry);
                if (!std::filesystem::is_directory(tagDir, ec)) continue;
                for (const auto& f : std::filesystem::recursive_directory_iterator(tagDir, ec)) {
                    if (!f.is_regular_file() || f.path().extension() != ".json") continue;
                    std::string rel = std::filesystem::relative(f.path(), tagDir, ec).generic_string();
                    rel = rel.substr(0, rel.size() - 5);   // strip .json
                    std::ifstream in(f.path());
                    if (!in) continue;
                    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false, true);
                    if (!j.is_object() || !j.contains("values") || !j["values"].is_array()) continue;
                    std::vector<std::string>& entries = index.rawTags[ns + ":" + rel];
                    // A tag file may `replace` an earlier pack's list; there is
                    // one pack here, so the values are simply appended.
                    for (const auto& v : j["values"]) {
                        if (v.is_string()) {
                            entries.push_back(v.get<std::string>());
                        } else if (v.is_object() && v.contains("id") && v["id"].is_string()) {
                            entries.push_back(v["id"].get<std::string>());
                        }
                    }
                }
            }
        }

        // Every concrete id a tag expands to, nested tags followed.
        void Expand(const Index& index, const std::string& tag, std::set<std::string>& out,
                    std::set<std::string>& visiting) {
            if (!visiting.insert(tag).second) return;   // self-including tag
            auto it = index.rawTags.find(tag);
            if (it == index.rawTags.end()) return;
            for (const std::string& raw : it->second) {
                std::string e = raw;
                bool optional = false;
                if (!e.empty() && e[0] == '#') {
                    e.erase(0, 1);
                    Expand(index, WithNamespace(e), out, visiting);
                } else {
                    (void)optional;
                    out.insert(WithNamespace(e));
                }
            }
        }

        void Build(Index& index, Registry registry) {
            index.loaded = true;
            ScanRegistry(index, registry);
            for (const auto& [tag, _] : index.rawTags) {
                std::set<std::string> ids, visiting;
                Expand(index, tag, ids, visiting);
                for (const std::string& id : ids) index.byId[id].push_back("#" + tag);
            }
            for (auto& [id, tags] : index.byId) std::sort(tags.begin(), tags.end());
            Log::Info("[DataTags] %s: %zu tags over %zu ids", RegistryDir(registry),
                      index.rawTags.size(), index.byId.size());
        }

    } // namespace

    const std::vector<std::string>& TagsFor(Registry registry, std::string_view id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Index& index = g_index[static_cast<int>(registry)];
        if (!index.loaded) Build(index, registry);
        auto it = index.byId.find(WithNamespace(std::string(id)));
        return it == index.byId.end() ? index.empty : it->second;
    }

    bool TagExists(Registry registry, std::string_view tag) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Index& index = g_index[static_cast<int>(registry)];
        if (!index.loaded) Build(index, registry);
        return index.rawTags.count(WithNamespace(std::string(tag))) != 0;
    }

    void Reload() {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Index& index : g_index) index = Index{};
    }

} // namespace Game::DataTags
