// File: src/common/advancements/AdvancementLoader.cpp
#include "AdvancementLoader.hpp"

#include "common/core/Log.hpp"
#include "common/world/crafting/RecipeManager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>

namespace Game::Advancements {

    namespace {

        std::filesystem::path DefaultDataRoot() {
            // The rule the terrain library, DataTags and /locate share.
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        // ItemStackTemplate without its components: the id and the count.
        ItemStack DecodeIconBasic(const nlohmann::json& icon) {
            std::string id;
            int count = 1;
            if (icon.is_string()) {
                id = icon.get<std::string>();
            } else if (icon.is_object()) {
                id = icon.value("id", std::string{});
                count = icon.value("count", 1);
            }
            if (const size_t colon = id.find(':'); colon != std::string::npos) {
                if (id.compare(0, colon, "minecraft") != 0) return {};
                id = id.substr(colon + 1);
            }
            const ItemID item = RecipeManager::ItemFromSlug(id);
            if (item == Items::Air) return {};
            return ItemStack(item, std::max(1, count));
        }

        // A Component in its data-pack form (string, list, object); a
        // missing or malformed one is an empty literal.
        Text::Component ReadComponent(const nlohmann::json& j) {
            if (auto c = Text::FromJson(j)) return *c;
            return Text::Component::Empty();
        }

        bool ParseDisplay(const nlohmann::json& j, const IconDecoder& decoder, DisplayInfo& out, std::string& error) {
            if (!j.is_object()) { error = "display is not an object"; return false; }
            auto icon = j.find("icon");
            if (icon == j.end()) { error = "display has no icon"; return false; }
            out.icon = decoder ? decoder(*icon) : DecodeIconBasic(*icon);
            auto title = j.find("title");
            auto description = j.find("description");
            if (title == j.end() || description == j.end()) { error = "display needs a title and a description"; return false; }
            out.title = ReadComponent(*title);
            out.description = ReadComponent(*description);
            if (auto bg = j.find("background"); bg != j.end() && bg->is_string()) {
                out.background = WithNamespace(bg->get<std::string>());
            }
            if (auto frame = j.find("frame"); frame != j.end() && frame->is_string()) {
                const auto type = FrameByName(frame->get<std::string>());
                if (!type) { error = "unknown frame " + frame->get<std::string>(); return false; }
                out.type = *type;
            }
            out.showToast      = j.value("show_toast", true);
            out.announceToChat = j.value("announce_to_chat", true);
            out.hidden         = j.value("hidden", false);
            return true;
        }

        bool ParseRewards(const nlohmann::json& j, Rewards& out) {
            if (!j.is_object()) return false;
            out.experience = j.value("experience", 0);
            auto readList = [&j](const char* key, std::vector<std::string>& list) {
                auto it = j.find(key);
                if (it == j.end()) return;
                if (it->is_array()) {
                    for (const auto& e : *it) if (e.is_string()) list.push_back(WithNamespace(e.get<std::string>()));
                } else if (it->is_string()) {
                    list.push_back(WithNamespace(it->get<std::string>()));
                }
            };
            readList("loot", out.loot);
            readList("recipes", out.recipes);
            if (auto fn = j.find("function"); fn != j.end() && fn->is_string()) out.function = WithNamespace(fn->get<std::string>());
            return true;
        }

        // Advancement.CODEC + validate (AdvancementRequirements.validate:
        // the requirements must name exactly the criteria).
        std::unique_ptr<Definition> ParseAdvancement(const std::string& id, const nlohmann::json& j,
                                                     const IconDecoder& decoder, std::string& error) {
            if (!j.is_object()) { error = "not an object"; return nullptr; }
            auto def = std::make_unique<Definition>();
            def->id = id;
            if (auto parent = j.find("parent"); parent != j.end() && parent->is_string()) {
                def->parent = WithNamespace(parent->get<std::string>());
            }
            if (auto display = j.find("display"); display != j.end()) {
                DisplayInfo info;
                if (!ParseDisplay(*display, decoder, info, error)) return nullptr;
                def->display = std::move(info);
            }
            auto criteria = j.find("criteria");
            if (criteria == j.end() || !criteria->is_object()) { error = "missing criteria"; return nullptr; }
            for (auto it = criteria->begin(); it != criteria->end(); ++it) {
                if (!it.value().is_object()) { error = "criterion " + it.key() + " is not an object"; return nullptr; }
                auto trigger = it.value().find("trigger");
                if (trigger == it.value().end() || !trigger->is_string()) {
                    error = "criterion " + it.key() + " has no trigger";
                    return nullptr;
                }
                CriterionDef c;
                c.name = it.key();
                c.trigger = WithNamespace(trigger->get<std::string>());
                if (auto cond = it.value().find("conditions"); cond != it.value().end()) {
                    c.conditions = std::make_shared<const nlohmann::json>(*cond);
                }
                def->criteria.push_back(std::move(c));
            }
            std::vector<std::string> names;
            names.reserve(def->criteria.size());
            for (const auto& c : def->criteria) names.push_back(c.name);
            if (auto req = j.find("requirements"); req != j.end() && req->is_array()) {
                for (const auto& set : *req) {
                    if (!set.is_array()) { error = "requirements entry is not a list"; return nullptr; }
                    std::vector<std::string> names2;
                    for (const auto& n : set) if (n.is_string()) names2.push_back(n.get<std::string>());
                    if (names2.empty() && names.empty()) { error = "Requirement entry cannot be empty"; return nullptr; }
                    def->requirements.sets.push_back(std::move(names2));
                }
            } else {
                def->requirements = Requirements::AllOf(names);
            }
            {
                const std::vector<std::string> referenced = def->requirements.Names();
                const std::set<std::string> expected(names.begin(), names.end());
                const std::set<std::string> got(referenced.begin(), referenced.end());
                if (expected != got) {
                    error = "Advancement completion requirements did not exactly match specified criteria";
                    return nullptr;
                }
            }
            if (auto rewards = j.find("rewards"); rewards != j.end()) ParseRewards(*rewards, def->rewards);
            def->sendsTelemetryEvent = j.value("sends_telemetry_event", false);
            return def;
        }

        // Only there to fill a recipe book (see the header).
        bool IsRecipeUnlock(const Definition& def) {
            return !def.display && !def.rewards.recipes.empty() && def.rewards.experience == 0 &&
                   def.rewards.loot.empty() && !def.rewards.function;
        }

    } // namespace

    const Definition* Registry::Get(std::string_view id) const {
        std::string key(id);
        if (key.find(':') == std::string::npos) key = "minecraft:" + key;
        auto it = byId.find(key);
        return it == byId.end() ? nullptr : it->second;
    }

    std::vector<std::string> Registry::Ids() const {
        std::vector<std::string> ids;
        ids.reserve(definitions.size());
        for (const auto& def : definitions) ids.push_back(def->id);
        return ids;
    }

    std::shared_ptr<const Registry> LoadRegistry(const IconDecoder& decoder, const std::filesystem::path& dataRootIn) {
        auto registry = std::make_shared<Registry>();
        const std::filesystem::path dataRoot = dataRootIn.empty() ? DefaultDataRoot() : dataRootIn;

        std::error_code ec;
        std::vector<std::pair<std::string, std::filesystem::path>> files;   // id, path
        if (std::filesystem::is_directory(dataRoot, ec)) {
            for (const auto& ns : std::filesystem::directory_iterator(dataRoot, ec)) {
                if (!ns.is_directory()) continue;
                const std::filesystem::path dir = ns.path() / "advancement";
                if (!std::filesystem::is_directory(dir, ec)) continue;
                const std::string nsName = ns.path().filename().string();
                for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file() || it->path().extension() != ".json") continue;
                    std::filesystem::path rel = std::filesystem::relative(it->path(), dir, ec);
                    rel.replace_extension();
                    files.emplace_back(nsName + ":" + rel.generic_string(), it->path());
                }
            }
        }
        std::sort(files.begin(), files.end());

        std::vector<std::unique_ptr<Definition>> parsed;
        size_t skippedRecipes = 0;
        for (const auto& [id, path] : files) {
            std::ifstream in(path);
            if (!in) continue;
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false, true);
            if (j.is_discarded()) {
                Log::Error("[Advancements] Couldn't parse %s", path.string().c_str());
                continue;
            }
            std::string error;
            auto def = ParseAdvancement(id, j, decoder, error);
            if (!def) {
                Log::Error("[Advancements] Couldn't load advancement %s: %s", id.c_str(), error.c_str());
                continue;
            }
            if (IsRecipeUnlock(*def)) { ++skippedRecipes; continue; }
            parsed.push_back(std::move(def));
        }

        // The tree decides what survives (a missing parent drops the child
        // and its whole subtree); definitions keep tree order.
        std::vector<const Definition*> raw;
        raw.reserve(parsed.size());
        for (const auto& d : parsed) raw.push_back(d.get());
        registry->tree.AddAll(raw);
        std::unordered_map<const Definition*, std::unique_ptr<Definition>*> owner;
        for (auto& d : parsed) owner[d.get()] = &d;
        for (const Node* node : registry->tree.Ordered()) {
            auto it = owner.find(node->def);
            if (it == owner.end()) continue;
            registry->byId[node->def->id] = node->def;
            registry->definitions.push_back(std::move(*it->second));
        }

        // ServerAdvancementManager.apply: lay out every root with a display.
        for (const Node* root : registry->tree.Roots()) {
            LayoutTree(*root, [](const Node& n) -> DisplayInfo* {
                return n.def && n.def->display ? const_cast<DisplayInfo*>(&*n.def->display) : nullptr;
            });
        }

        Log::Info("[Advancements] Loaded %zu advancements (%zu recipe unlocks skipped: no recipe book)",
                  registry->definitions.size(), skippedRecipes);
        return registry;
    }

    const Registry& SuggestionRegistry() {
        static std::once_flag once;
        static std::shared_ptr<const Registry> registry;
        std::call_once(once, [] { registry = LoadRegistry(); });
        return *registry;
    }

} // namespace Game::Advancements
