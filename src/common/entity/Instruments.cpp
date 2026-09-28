// File: src/common/entity/Instruments.cpp
#include "common/entity/Instruments.hpp"

#include "common/core/Log.hpp"
#include "common/text/Language.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace Game::Instruments {

    namespace {

        // Same rule as ChestLoot and DataTags: MC_DATA_ROOT, else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        // "minecraft:item.goat_horn.sound.0" -> "item.goat_horn.sound.0";
        // another namespace's event keeps its prefix.
        std::string StripMinecraft(const std::string& id) {
            constexpr const char kPrefix[] = "minecraft:";
            return id.rfind(kPrefix, 0) == 0 ? id.substr(sizeof(kPrefix) - 1) : id;
        }

        std::filesystem::path PathFor(const std::string& key, const char* folder) {
            const size_t colon = key.find(':');
            const std::string ns   = key.substr(0, colon);
            const std::string path = key.substr(colon + 1);
            return DataRoot() / ns / folder / (path + ".json");
        }

        bool ReadJson(const std::filesystem::path& file, nlohmann::json& out) {
            std::ifstream in(file);
            if (!in) return false;
            try {
                in >> out;
                return true;
            } catch (const std::exception& e) {
                Log::Warning("[Instruments] %s: %s", file.string().c_str(), e.what());
                return false;
            }
        }

        std::mutex g_mutex;
        std::unordered_map<std::string, std::unique_ptr<Instrument>> g_instruments;

        // Caller holds g_mutex. Instrument.DIRECT_CODEC: sound_event (a
        // SoundEvent holder: an id, or {sound_id, range?}), use_duration
        // (positive float), range (positive float), description (Component).
        const Instrument* Lookup(const std::string& rawId) {
            const std::string id = WithNamespace(rawId);
            auto it = g_instruments.find(id);
            if (it != g_instruments.end()) return it->second.get();

            std::unique_ptr<Instrument> instrument;
            nlohmann::json j;
            if (ReadJson(PathFor(id, "instrument"), j) && j.is_object()) {
                auto def = std::make_unique<Instrument>();
                def->id = id;
                if (j.contains("sound_event")) {
                    const nlohmann::json& sound = j["sound_event"];
                    if (sound.is_string()) {
                        def->soundEvent = StripMinecraft(sound.get<std::string>());
                    } else if (sound.is_object() && sound.contains("sound_id") && sound["sound_id"].is_string()) {
                        def->soundEvent = StripMinecraft(sound["sound_id"].get<std::string>());
                    }
                }
                def->useDuration = j.value("use_duration", 0.0f);
                def->range       = j.value("range", 0.0f);
                if (j.contains("description")) {
                    const nlohmann::json& d = j["description"];
                    if (d.is_string()) def->descriptionKey = d.get<std::string>();
                    else if (d.is_object()) {
                        if (d.contains("translate") && d["translate"].is_string()) {
                            def->descriptionKey = d["translate"].get<std::string>();
                        } else if (d.contains("text") && d["text"].is_string()) {
                            def->descriptionKey = d["text"].get<std::string>();
                        }
                    }
                }
                if (!def->soundEvent.empty() && def->useDuration > 0.0f && def->range > 0.0f) {
                    instrument = std::move(def);
                } else {
                    Log::Warning("[Instruments] %s: incomplete definition", id.c_str());
                }
            }
            // A definition that could not be read is not remembered: the
            // data root may simply not have been reachable yet.
            if (!instrument) return nullptr;
            const Instrument* result = instrument.get();
            g_instruments[id] = std::move(instrument);
            return result;
        }

        // TagEntry: "id", "#tag", or {id, required}. Nested tags recurse;
        // `visiting` stops a cycle.
        void ExpandTag(const std::string& tag, std::vector<std::string>& out,
                       std::set<std::string>& visiting) {
            if (!visiting.insert(tag).second) return;
            nlohmann::json j;
            if (!ReadJson(PathFor(tag, "tags/instrument"), j) || !j.is_object()) return;
            if (!j.contains("values") || !j["values"].is_array()) return;
            for (const nlohmann::json& entry : j["values"]) {
                std::string value;
                if (entry.is_string()) value = entry.get<std::string>();
                else if (entry.is_object() && entry.contains("id") && entry["id"].is_string()) {
                    value = entry["id"].get<std::string>();
                }
                if (value.empty()) continue;
                if (value[0] == '#') {
                    ExpandTag(WithNamespace(value.substr(1)), out, visiting);
                } else {
                    const std::string id = WithNamespace(value);
                    bool seen = false;
                    for (const std::string& existing : out) seen = seen || existing == id;
                    if (!seen) out.push_back(id);
                }
            }
        }

    } // namespace

    const Instrument* Get(const std::string& id) {
        if (id.empty()) return nullptr;
        std::lock_guard<std::mutex> lock(g_mutex);
        return Lookup(id);
    }

    std::vector<std::string> Resolve(const std::string& spec) {
        std::vector<std::string> ids;
        if (spec.empty()) return ids;
        if (spec[0] == '#') {
            std::set<std::string> visiting;
            ExpandTag(WithNamespace(spec.substr(1)), ids, visiting);
        } else {
            ids.push_back(WithNamespace(spec));
        }
        // Registry holders only: an entry the registry lacks is no holder.
        std::vector<std::string> out;
        for (const std::string& id : ids) {
            if (Get(id)) out.push_back(id);
        }
        return out;
    }

    std::string DescriptionOf(const std::string& id) {
        const Instrument* instrument = Get(id);
        if (!instrument || instrument->descriptionKey.empty()) return {};
        return Language::Get(instrument->descriptionKey);
    }

    void Reload() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instruments.clear();
    }

} // namespace Game::Instruments
