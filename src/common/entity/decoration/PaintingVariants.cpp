// File: src/common/entity/decoration/PaintingVariants.cpp
#include "common/entity/decoration/PaintingVariants.hpp"
#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace Game::PaintingVariants {

    namespace {
        // Same rule as DataTags and the enchantment definitions: MC_DATA_ROOT,
        // else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        std::string WithNamespace(std::string_view id) {
            if (id.find(':') != std::string_view::npos) return std::string(id);
            return "minecraft:" + std::string(id);
        }

        std::optional<PaintingVariant::TextLine> ReadLine(const nlohmann::json& j, const char* key) {
            if (!j.contains(key)) return std::nullopt;
            const auto& line = j.at(key);
            PaintingVariant::TextLine out;
            if (line.is_string()) {
                out.translate = line.get<std::string>();   // a plain-text component
            } else if (line.is_object()) {
                out.translate = line.value("translate", line.value("text", std::string{}));
                out.color     = line.value("color", std::string{});
            }
            if (out.translate.empty()) return std::nullopt;
            return out;
        }

        struct Registry {
            std::vector<PaintingVariant> all;
            std::vector<int> placeable;
        };

        // data/<ns>/tags/painting_variant/<path>.json, nested tags followed.
        void CollectTag(const std::string& tag, std::vector<std::string>& out, std::set<std::string>& seen) {
            const std::string id = WithNamespace(tag);
            if (!seen.insert(id).second) return;
            const size_t colon = id.find(':');
            const auto file = DataRoot() / id.substr(0, colon) / "tags" / "painting_variant" /
                              (id.substr(colon + 1) + ".json");
            std::ifstream in(file);
            if (!in) return;
            try {
                const nlohmann::json j = nlohmann::json::parse(in);
                for (const auto& v : j.value("values", nlohmann::json::array())) {
                    const std::string entry = v.is_string() ? v.get<std::string>()
                                                            : v.value("id", std::string{});
                    if (entry.empty()) continue;
                    if (entry[0] == '#') CollectTag(entry.substr(1), out, seen);
                    else out.push_back(WithNamespace(entry));
                }
            } catch (const std::exception& e) {
                Log::Warning("[Painting] bad tag %s: %s", file.string().c_str(), e.what());
            }
        }

        Registry Load() {
            Registry reg;
            std::error_code ec;
            for (const auto& ns : std::filesystem::directory_iterator(DataRoot(), ec)) {
                if (!ns.is_directory(ec)) continue;
                const auto dir = ns.path() / "painting_variant";
                if (!std::filesystem::is_directory(dir, ec)) continue;
                for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
                    if (file.path().extension() != ".json") continue;
                    std::ifstream in(file.path());
                    try {
                        const nlohmann::json j = nlohmann::json::parse(in);
                        PaintingVariant v;
                        v.id      = ns.path().filename().string() + ":" + file.path().stem().string();
                        v.assetId = WithNamespace(j.value("asset_id", v.id));
                        // MC PaintingVariant.DIRECT_CODEC: width / height in 1..16.
                        v.width   = std::clamp(j.value("width", 1), 1, 16);
                        v.height  = std::clamp(j.value("height", 1), 1, 16);
                        v.title   = ReadLine(j, "title");
                        v.author  = ReadLine(j, "author");
                        reg.all.push_back(std::move(v));
                    } catch (const std::exception& e) {
                        Log::Warning("[Painting] bad variant %s: %s", file.path().string().c_str(), e.what());
                    }
                }
            }
            std::sort(reg.all.begin(), reg.all.end(),
                      [](const PaintingVariant& a, const PaintingVariant& b) { return a.id < b.id; });

            std::vector<std::string> ids;
            std::set<std::string> seen;
            CollectTag("minecraft:placeable", ids, seen);
            for (const std::string& id : ids) {
                const auto it = std::lower_bound(reg.all.begin(), reg.all.end(), id,
                                                 [](const PaintingVariant& v, const std::string& key) { return v.id < key; });
                if (it != reg.all.end() && it->id == id) {
                    reg.placeable.push_back(static_cast<int>(it - reg.all.begin()));
                }
            }
            Log::Info("[Painting] %zu variants, %zu placeable", reg.all.size(), reg.placeable.size());
            return reg;
        }

        const Registry& TheRegistry() {
            static const Registry reg = Load();
            return reg;
        }
    }

    const std::vector<PaintingVariant>& All() { return TheRegistry().all; }

    const PaintingVariant* Get(int index) {
        const auto& all = All();
        return index >= 0 && index < static_cast<int>(all.size()) ? &all[static_cast<size_t>(index)] : nullptr;
    }

    int IndexOf(std::string_view id) {
        const std::string key = WithNamespace(id);
        const auto& all = All();
        const auto it = std::lower_bound(all.begin(), all.end(), key,
                                         [](const PaintingVariant& v, const std::string& k) { return v.id < k; });
        return it != all.end() && it->id == key ? static_cast<int>(it - all.begin()) : -1;
    }

    const std::vector<int>& Placeable() { return TheRegistry().placeable; }

    std::string TexturePath(const PaintingVariant& variant) {
        const size_t colon = variant.assetId.find(':');
        const std::string path = colon == std::string::npos ? variant.assetId : variant.assetId.substr(colon + 1);
        return "assets/textures/painting/" + path + ".png";
    }

} // namespace Game::PaintingVariants
