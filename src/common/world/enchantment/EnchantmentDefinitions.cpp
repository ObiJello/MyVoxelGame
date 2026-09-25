// File: src/common/world/enchantment/EnchantmentDefinitions.cpp
#include "EnchantmentDefinitions.hpp"

#include "common/core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>

namespace Game::EnchantmentDefinitions {

    namespace {

        // Same rule as DataTags and the terrain library: MC_DATA_ROOT, else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        // "minecraft:sharpness" -> "sharpness"; "sharpness" unchanged.
        std::string_view StripNamespace(std::string_view id) {
            const size_t colon = id.find(':');
            return colon == std::string_view::npos ? id : id.substr(colon + 1);
        }

        struct Index {
            bool loaded = false;
            std::vector<Definition> byId;                 // indexed by EnchantmentId
            std::vector<EnchantmentId> all;
            std::vector<EnchantmentId> tooltipOrder;      // #minecraft:tooltip_order, tag order
            // enchantment tag ("minecraft:on_random_loot") -> raw entries
            std::unordered_map<std::string, std::vector<std::string>> rawTags;
        };

        Index g_index;
        std::mutex g_mutex;

        // A HolderSet field: string or array of strings.
        std::vector<std::string> ReadSet(const nlohmann::json& j) {
            std::vector<std::string> out;
            if (j.is_string()) {
                out.push_back(j.get<std::string>());
            } else if (j.is_array()) {
                for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
            }
            return out;
        }

        Cost ReadCost(const nlohmann::json& j, Cost fallback) {
            if (!j.is_object()) return fallback;
            Cost c;
            c.base = j.value("base", fallback.base);
            c.perLevelAboveFirst = j.value("per_level_above_first", fallback.perLevelAboveFirst);
            return c;
        }

        void ScanTags(Index& index) {
            const std::filesystem::path root = DataRoot();
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) return;
            for (const auto& nsEntry : std::filesystem::directory_iterator(root, ec)) {
                if (!nsEntry.is_directory()) continue;
                const std::string ns = nsEntry.path().filename().string();
                const std::filesystem::path tagDir = nsEntry.path() / "tags" / "enchantment";
                if (!std::filesystem::is_directory(tagDir, ec)) continue;
                for (const auto& f : std::filesystem::recursive_directory_iterator(tagDir, ec)) {
                    if (!f.is_regular_file() || f.path().extension() != ".json") continue;
                    std::ifstream in(f.path());
                    if (!in) continue;
                    nlohmann::json j;
                    try { in >> j; } catch (...) { continue; }
                    const std::string rel = std::filesystem::relative(f.path(), tagDir, ec)
                                                .replace_extension().generic_string();
                    std::vector<std::string> values;
                    if (j.contains("values") && j["values"].is_array()) {
                        for (const auto& v : j["values"]) {
                            if (v.is_string()) values.push_back(v.get<std::string>());
                            else if (v.is_object() && v.contains("id") && v["id"].is_string())
                                values.push_back(v["id"].get<std::string>());
                        }
                    }
                    index.rawTags[ns + ":" + rel] = std::move(values);
                }
            }
        }

        // A named HolderSet<Enchantment> in its own order: the tag file's
        // entries first to last, a nested #tag expanded in place, each
        // enchantment once (MC HolderSet.Named iterates its resolved
        // contents, which is tag order — not registry order).
        std::vector<EnchantmentId> OrderedTag(const Index& index, const std::string& tag) {
            std::vector<EnchantmentId> out;
            std::set<std::string> seen;
            std::function<void(const std::string&)> walk = [&](const std::string& entry) {
                if (entry.empty()) return;
                if (entry[0] == '#') {
                    const std::string t = WithNamespace(entry.substr(1));
                    if (!seen.insert(t).second) return;
                    auto it = index.rawTags.find(t);
                    if (it == index.rawTags.end()) return;
                    for (const std::string& child : it->second) walk(child);
                    return;
                }
                if (auto id = EnchantmentRegistry::ByName(StripNamespace(WithNamespace(entry)))) {
                    if (std::find(out.begin(), out.end(), *id) == out.end()) out.push_back(*id);
                }
            };
            walk("#" + WithNamespace(tag));
            return out;
        }

        void Load(Index& index) {
            index.loaded = true;
            const auto& registry = EnchantmentRegistry::All();
            index.byId.assign(registry.size(), Definition{});
            ScanTags(index);

            const std::filesystem::path dir = DataRoot() / "minecraft" / "enchantment";
            size_t loaded = 0;
            for (size_t i = 0; i < registry.size(); ++i) {
                const std::filesystem::path file = dir / (registry[i].slug + ".json");
                std::ifstream in(file);
                if (!in) continue;
                nlohmann::json j;
                try { in >> j; } catch (const std::exception& e) {
                    Log::Warning("[Enchantments] %s: %s", file.string().c_str(), e.what());
                    continue;
                }
                Definition d;
                d.loaded   = true;
                d.weight   = j.value("weight", 0);
                d.maxLevel = j.value("max_level", registry[i].maxLevel);
                d.minCost  = ReadCost(j.contains("min_cost") ? j["min_cost"] : nlohmann::json(), Cost{1, 0});
                d.maxCost  = ReadCost(j.contains("max_cost") ? j["max_cost"] : nlohmann::json(), Cost{50, 0});
                if (j.contains("supported_items")) d.supportedItems = ReadSet(j["supported_items"]);
                if (j.contains("primary_items"))   d.primaryItems   = ReadSet(j["primary_items"]);
                d.anvilCost = j.value("anvil_cost", 0);
                if (j.contains("slots")) d.slots = ReadSet(j["slots"]);
                if (j.contains("effects") && j["effects"].is_object()) {
                    d.effects = EnchantmentEffectComponents::Parse(j["effects"], registry[i].slug);
                }
                index.byId[i] = std::move(d);
                index.all.push_back(static_cast<EnchantmentId>(i));
                ++loaded;
            }
            // exclusive_set needs every id known first (a set may name a tag).
            for (size_t i = 0; i < registry.size(); ++i) {
                if (!index.byId[i].loaded) continue;
                const std::filesystem::path file = dir / (registry[i].slug + ".json");
                std::ifstream in(file);
                nlohmann::json j;
                try { in >> j; } catch (...) { continue; }
                if (!j.contains("exclusive_set")) continue;
                // Resolve against the index being built (ResolveSet locks, so
                // do it inline here).
                std::vector<std::string> entries = ReadSet(j["exclusive_set"]);
                std::vector<EnchantmentId> ids;
                std::set<std::string> seen;
                std::vector<std::string> work(entries.begin(), entries.end());
                while (!work.empty()) {
                    std::string e = std::move(work.back());
                    work.pop_back();
                    if (e.empty()) continue;
                    if (e[0] == '#') {
                        const std::string tag = WithNamespace(e.substr(1));
                        if (!seen.insert("#" + tag).second) continue;
                        auto it = index.rawTags.find(tag);
                        if (it != index.rawTags.end()) work.insert(work.end(), it->second.begin(), it->second.end());
                        continue;
                    }
                    if (auto id = EnchantmentRegistry::ByName(StripNamespace(WithNamespace(e)))) ids.push_back(*id);
                }
                index.byId[i].exclusiveSet = std::move(ids);
            }
            index.tooltipOrder = OrderedTag(index, "minecraft:tooltip_order");
            Log::Info("[Enchantments] %zu of %zu enchantment definitions loaded from %s",
                      loaded, registry.size(), dir.string().c_str());
        }

        Index& Loaded() {
            if (!g_index.loaded) Load(g_index);
            return g_index;
        }

        // MC HolderSet.contains(item.typeHolder()) over a raw entry list.
        bool SetContainsItem(const std::vector<std::string>& entries, ItemID item) {
            return ItemHolderSetContains(entries, item);
        }

        // MC EquipmentSlotGroup.test(slot) for one group name.
        bool SlotGroupContains(std::string_view group, EquipmentSlot slot) {
            group = StripNamespace(group);
            switch (slot) {
                case EquipmentSlot::MAINHAND:
                    return group == "any" || group == "hand" || group == "mainhand";
                case EquipmentSlot::OFFHAND:
                    return group == "any" || group == "hand" || group == "offhand";
                case EquipmentSlot::FEET:
                    return group == "any" || group == "armor" || group == "feet";
                case EquipmentSlot::LEGS:
                    return group == "any" || group == "armor" || group == "legs";
                case EquipmentSlot::CHEST:
                    return group == "any" || group == "armor" || group == "chest";
                case EquipmentSlot::HEAD:
                    return group == "any" || group == "armor" || group == "head";
                // EquipmentSlot.isArmor: BODY is ANIMAL_ARMOR, so "armor"
                // holds it too; "any" holds every slot, the saddle included.
                case EquipmentSlot::BODY:
                    return group == "any" || group == "armor" || group == "body";
                case EquipmentSlot::SADDLE:
                    return group == "any" || group == "saddle";
            }
            return false;
        }

    } // namespace

    const Definition& Get(EnchantmentId id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Index& index = Loaded();
        static const Definition kNone{};
        return id < index.byId.size() ? index.byId[id] : kNone;
    }

    bool IsSupportedItem(EnchantmentId id, ItemID item) {
        const Definition& d = Get(id);
        return d.loaded && SetContainsItem(d.supportedItems, item);
    }

    bool IsPrimaryItem(EnchantmentId id, ItemID item) {
        const Definition& d = Get(id);
        if (!d.loaded || !SetContainsItem(d.supportedItems, item)) return false;
        return d.primaryItems.empty() || SetContainsItem(d.primaryItems, item);
    }

    bool AreCompatible(EnchantmentId a, EnchantmentId b) {
        if (a == b) return false;
        const auto& ea = Get(a).exclusiveSet;
        const auto& eb = Get(b).exclusiveSet;
        return std::find(ea.begin(), ea.end(), b) == ea.end()
            && std::find(eb.begin(), eb.end(), a) == eb.end();
    }

    void ModifyDurabilityChange(EnchantmentId id, int level, const ItemStack& item,
                                JavaRandom& random, float& value) {
        const Definition& d = Get(id);
        if (!d.loaded) return;
        // Enchantment.itemContext: TOOL + ENCHANTMENT_LEVEL.
        EnchantmentContext ctx;
        ctx.random = &random;
        ctx.enchantmentLevel = level;
        ctx.tool = &item;
        for (const ConditionalValueEffect& effect : d.effects.itemDamage) {
            if (effect.Matches(ctx)) value = effect.effect.Process(level, random, value);
        }
    }

    bool MatchingSlot(EnchantmentId id, EquipmentSlot slot) {
        const Definition& d = Get(id);
        for (const std::string& group : d.slots) {
            if (SlotGroupContains(group, slot)) return true;
        }
        return false;
    }

    std::vector<EnchantmentId> ResolveTagOrdered(std::string_view tag) {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::string t(tag);
        if (!t.empty() && t[0] == '#') t.erase(0, 1);
        return OrderedTag(Loaded(), t);
    }

    const std::vector<EnchantmentId>& TooltipOrder() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return Loaded().tooltipOrder;
    }

    std::vector<EnchantmentId> ResolveSet(const std::vector<std::string>& entries) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Index& index = Loaded();
        std::vector<EnchantmentId> ids;
        std::set<std::string> seen;
        std::vector<std::string> work(entries.begin(), entries.end());
        while (!work.empty()) {
            std::string e = std::move(work.back());
            work.pop_back();
            if (e.empty()) continue;
            if (e[0] == '#') {
                const std::string tag = WithNamespace(e.substr(1));
                if (!seen.insert("#" + tag).second) continue;
                auto it = index.rawTags.find(tag);
                if (it != index.rawTags.end()) work.insert(work.end(), it->second.begin(), it->second.end());
                continue;
            }
            if (auto id = EnchantmentRegistry::ByName(StripNamespace(WithNamespace(e)))) {
                if (*id < index.byId.size() && index.byId[*id].loaded
                    && std::find(ids.begin(), ids.end(), *id) == ids.end()) {
                    ids.push_back(*id);
                }
            }
        }
        // Registry order, like a HolderSet iterated from the registry.
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    std::vector<EnchantmentId> ResolveTag(std::string_view tag) {
        std::string t(tag);
        if (t.empty() || t[0] != '#') t = "#" + t;
        return ResolveSet({t});
    }

    const std::vector<EnchantmentId>& All() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return Loaded().all;
    }

    void Reload() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_index = Index{};
    }

} // namespace Game::EnchantmentDefinitions
