// File: src/common/entity/npc/VillagerTrades.cpp
#include "common/entity/npc/VillagerTrades.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/text/Language.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game::VillagerTrades {

    namespace {

        // Same rule as ChestLootTables / DataTags: MC_DATA_ROOT, else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        std::string StripNs(const std::string& id) {
            const size_t colon = id.find(':');
            return colon == std::string::npos ? id : id.substr(colon + 1);
        }

        // "minecraft:farmer/level_1" → (minecraft, farmer/level_1).
        std::pair<std::string, std::string> SplitId(const std::string& id) {
            const size_t colon = id.find(':');
            if (colon == std::string::npos) return { "minecraft", id };
            return { id.substr(0, colon), id.substr(colon + 1) };
        }

        bool ReadJson(const std::filesystem::path& path, nlohmann::json& out) {
            std::ifstream f(path);
            if (!f) return false;
            try {
                out = nlohmann::json::parse(f);
                return true;
            } catch (const std::exception& e) {
                Log::Warning("[VillagerTrades] %s: %s", path.string().c_str(), e.what());
                return false;
            }
        }

        ItemID ItemFromId(const std::string& id) {
            return RecipeManager::ItemFromSlug(StripNs(id));
        }

        // ── Number providers (MC ContextIntProvider / ContextFloatProvider) ─
        // The four the trade files use: a bare number (ConstantValue's
        // inline form), minecraft:constant, minecraft:uniform (Mth.nextInt
        // over the floors, inclusive), minecraft:binomial (n draws at p) and
        // minecraft:sum (the summands' total — set_random_dyes' count).
        struct Number {
            enum class Kind : uint8_t { Constant, Uniform, Binomial, Sum };
            Kind kind = Kind::Constant;
            float value = 0.0f;
            std::vector<Number> args;   // uniform: [min, max]; binomial: [n, p]; sum: summands

            int GetInt(JavaRandom& r) const {
                switch (kind) {
                    case Kind::Constant: return static_cast<int>(std::floor(value));
                    case Kind::Uniform: {
                        const int lo = args.size() > 0 ? args[0].GetInt(r) : 0;
                        const int hi = args.size() > 1 ? args[1].GetInt(r) : 0;
                        return lo >= hi ? lo : lo + r.NextInt(hi - lo + 1);
                    }
                    case Kind::Binomial: {
                        const int n = args.size() > 0 ? args[0].GetInt(r) : 0;
                        const float p = args.size() > 1 ? args[1].GetFloat(r) : 0.0f;
                        int k = 0;
                        for (int i = 0; i < n; ++i) if (r.NextFloat() < p) ++k;
                        return k;
                    }
                    case Kind::Sum: {
                        int64_t total = 0;
                        for (const Number& a : args) total += a.GetInt(r);
                        return static_cast<int>(std::clamp<int64_t>(total, INT32_MIN, INT32_MAX));
                    }
                }
                return 0;
            }
            float GetFloat(JavaRandom& r) const {
                switch (kind) {
                    case Kind::Constant: return value;
                    case Kind::Uniform: {
                        const float lo = args.size() > 0 ? args[0].GetFloat(r) : 0.0f;
                        const float hi = args.size() > 1 ? args[1].GetFloat(r) : 0.0f;
                        return lo + r.NextFloat() * (hi - lo);
                    }
                    case Kind::Binomial:
                    case Kind::Sum:
                        return static_cast<float>(GetInt(r));
                }
                return 0.0f;
            }
            static Number Constant(float v) { Number n; n.value = v; return n; }
        };

        Number ParseNumber(const nlohmann::json& j, float fallback) {
            if (j.is_number()) return Number::Constant(j.get<float>());
            if (!j.is_object()) return Number::Constant(fallback);
            const std::string type = StripNs(j.value("type", std::string("constant")));
            Number n;
            if (type == "uniform") {
                n.kind = Number::Kind::Uniform;
                n.args.push_back(j.contains("min") ? ParseNumber(j["min"], 0.0f) : Number::Constant(0.0f));
                n.args.push_back(j.contains("max") ? ParseNumber(j["max"], 0.0f) : Number::Constant(0.0f));
            } else if (type == "binomial") {
                n.kind = Number::Kind::Binomial;
                n.args.push_back(j.contains("n") ? ParseNumber(j["n"], 0.0f) : Number::Constant(0.0f));
                n.args.push_back(j.contains("p") ? ParseNumber(j["p"], 0.0f) : Number::Constant(0.0f));
            } else if (type == "sum") {
                n.kind = Number::Kind::Sum;
                if (j.contains("summands") && j["summands"].is_array()) {
                    for (const auto& s : j["summands"]) n.args.push_back(ParseNumber(s, 0.0f));
                }
            } else {
                n = Number::Constant(j.contains("value") && j["value"].is_number()
                                         ? j["value"].get<float>() : fallback);
            }
            return n;
        }

        std::vector<std::string> ReadStringOrList(const nlohmann::json& j) {
            std::vector<std::string> out;
            if (j.is_string()) out.push_back(j.get<std::string>());
            else if (j.is_array()) for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
            return out;
        }

        // A potion HolderSet: "#minecraft:tradeable" (data/<ns>/tags/potion),
        // an id, or a list of either. Nested tags are followed.
        void ResolvePotionSet(const std::vector<std::string>& entries, std::vector<PotionId>& out,
                              std::set<std::string>& visiting) {
            for (const std::string& e : entries) {
                if (!e.empty() && e[0] == '#') {
                    const std::string tag = e.substr(1);
                    if (!visiting.insert(tag).second) continue;
                    const auto [ns, path] = SplitId(tag);
                    nlohmann::json j;
                    if (ReadJson(DataRoot() / ns / "tags" / "potion" / (path + ".json"), j) &&
                        j.contains("values")) {
                        std::vector<std::string> values;
                        for (const auto& v : j["values"]) {
                            if (v.is_string()) values.push_back(v.get<std::string>());
                            else if (v.is_object() && v.contains("id")) values.push_back(v["id"].get<std::string>());
                        }
                        ResolvePotionSet(values, out, visiting);
                    }
                    continue;
                }
                PotionId id;
                if (ParsePotionId(e, id) &&
                    std::find(out.begin(), out.end(), id) == out.end()) {
                    out.push_back(id);
                }
            }
        }

        // The component forms the trade files carry on a wanted or given
        // stack — MC DataComponentExactPredicate / ItemStackTemplate. Only
        // the components the engine can represent are read.
        DataComponentMap ParseComponents(const nlohmann::json& j) {
            DataComponentMap map;
            if (!j.is_object()) return map;
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string key = StripNs(it.key());
                const auto& v = it.value();
                if (key == "potion_contents") {
                    // PotionContents.CODEC: the full compound or a bare id.
                    std::string potion;
                    if (v.is_string()) potion = v.get<std::string>();
                    else if (v.is_object()) potion = v.value("potion", std::string());
                    PotionId id;
                    if (!potion.empty() && ParsePotionId(potion, id)) {
                        map.set(DataComponents::POTION_CONTENTS, PotionContents{}.WithPotion(id));
                    }
                } else if (key == "dyed_color" && v.is_number_integer()) {
                    map.set(DataComponents::DYED_COLOR, static_cast<int32_t>(v.get<int64_t>()));
                } else if (key == "custom_name" && v.is_string()) {
                    map.set(DataComponents::CUSTOM_NAME, v.get<std::string>());
                }
            }
            return map;
        }

        // ── Parsed trade ──────────────────────────────────────────────────

        // MC TradeCost.
        struct Cost {
            ItemID           item = Items::Air;
            Number           count = Number::Constant(1.0f);
            DataComponentMap components;
        };

        // MC LootItemFunction, the subset the trades use.
        struct Function {
            enum class Kind : uint8_t {
                EnchantRandomly, EnchantWithLevels, Filtered, Discard, SetName,
                SetPotion, SetRandomPotion, SetStewEffect, SetRandomDyes,
                ExplorationMap, Unsupported,
            };
            Kind kind = Kind::Unsupported;
            std::string name;
            // enchant_*
            bool hasOptions = false;
            std::vector<EnchantmentId> options;
            bool onlyCompatible = true;
            bool includeAdditionalCost = false;
            Number levels;
            // filtered
            std::vector<ItemID> filterItems;
            bool needsEnchantments = false, needsStoredEnchantments = false;
            bool needsDyedColor = false, needsMapId = false;
            std::vector<Function> onPass, onFail;
            // set_name
            std::string text;
            bool itemName = false;
            // set_potion / set_random_potion
            PotionId potion = PotionId::Water;
            std::vector<PotionId> potions;
            bool hasPotionOptions = false;
            // set_stew_effect
            std::vector<std::pair<MobEffectId, Number>> stewEffects;
            // set_random_dyes
            Number dyes;
        };

        std::vector<Function> ParseFunctions(const nlohmann::json& j);

        Function ParseFunction(const nlohmann::json& f) {
            Function fn;
            fn.name = StripNs(f.value("function", std::string()));
            if (fn.name == "enchant_randomly" || fn.name == "enchant_with_levels") {
                fn.kind = fn.name == "enchant_randomly" ? Function::Kind::EnchantRandomly
                                                        : Function::Kind::EnchantWithLevels;
                if (f.contains("options")) {
                    fn.hasOptions = true;
                    fn.options = EnchantmentDefinitions::ResolveSet(ReadStringOrList(f["options"]));
                }
                fn.onlyCompatible = f.value("only_compatible", true);
                fn.includeAdditionalCost = f.value("include_additional_cost_component", false);
                if (f.contains("levels")) fn.levels = ParseNumber(f["levels"], 1.0f);
            } else if (fn.name == "filtered") {
                fn.kind = Function::Kind::Filtered;
                if (f.contains("item_filter") && f["item_filter"].is_object()) {
                    const auto& filter = f["item_filter"];
                    if (filter.contains("items")) {
                        for (const std::string& id : ReadStringOrList(filter["items"])) {
                            if (!id.empty() && id[0] == '#') continue;   // item tags: none in the trades
                            const ItemID item = ItemFromId(id);
                            if (item != Items::Air) fn.filterItems.push_back(item);
                        }
                    }
                    if (filter.contains("predicates") && filter["predicates"].is_object()) {
                        for (auto it = filter["predicates"].begin(); it != filter["predicates"].end(); ++it) {
                            const std::string key = StripNs(it.key());
                            if (key == "enchantments")        fn.needsEnchantments = true;
                            else if (key == "stored_enchantments") fn.needsStoredEnchantments = true;
                            else if (key == "dyed_color")     fn.needsDyedColor = true;
                            else if (key == "map_id")         fn.needsMapId = true;
                        }
                    }
                }
                if (f.contains("on_pass")) fn.onPass = ParseFunctions(f["on_pass"]);
                if (f.contains("on_fail")) fn.onFail = ParseFunctions(f["on_fail"]);
            } else if (fn.name == "discard") {
                fn.kind = Function::Kind::Discard;
            } else if (fn.name == "set_name") {
                fn.kind = Function::Kind::SetName;
                if (f.contains("name")) {
                    const auto& n = f["name"];
                    if (n.is_string()) fn.text = n.get<std::string>();
                    else if (n.is_object() && n.contains("translate")) fn.text = Language::Get(n["translate"].get<std::string>());
                    else if (n.is_object() && n.contains("text")) fn.text = n["text"].get<std::string>();
                }
                fn.itemName = StripNs(f.value("target", std::string("custom_name"))) == "item_name";
            } else if (fn.name == "set_potion") {
                PotionId id;
                if (ParsePotionId(f.value("id", std::string()), id)) {
                    fn.kind = Function::Kind::SetPotion;
                    fn.potion = id;
                }
            } else if (fn.name == "set_random_potion") {
                fn.kind = Function::Kind::SetRandomPotion;
                if (f.contains("options")) {
                    fn.hasPotionOptions = true;
                    std::set<std::string> visiting;
                    ResolvePotionSet(ReadStringOrList(f["options"]), fn.potions, visiting);
                }
            } else if (fn.name == "set_stew_effect") {
                fn.kind = Function::Kind::SetStewEffect;
                if (f.contains("effects") && f["effects"].is_array()) {
                    for (const auto& e : f["effects"]) {
                        MobEffectId id;
                        if (!e.is_object() || !ParseEffectId(e.value("type", std::string()), id)) continue;
                        fn.stewEffects.emplace_back(id, e.contains("duration") ? ParseNumber(e["duration"], 1.0f)
                                                                                : Number::Constant(1.0f));
                    }
                }
            } else if (fn.name == "set_random_dyes") {
                fn.kind = Function::Kind::SetRandomDyes;
                fn.dyes = f.contains("number_of_dyes") ? ParseNumber(f["number_of_dyes"], 1.0f)
                                                       : Number::Constant(1.0f);
            } else if (fn.name == "exploration_map") {
                fn.kind = Function::Kind::ExplorationMap;
            } else if (fn.name == "sequence" && f.contains("functions")) {
                // A LootItemFunctions sequence written as an object.
                Function seq;
                seq.kind = Function::Kind::Filtered;   // reuse onPass as the body
                seq.name = "sequence";
                seq.onPass = ParseFunctions(f["functions"]);
                return seq;
            }
            return fn;
        }

        std::vector<Function> ParseFunctions(const nlohmann::json& j) {
            std::vector<Function> out;
            if (j.is_array()) {
                for (const auto& f : j) if (f.is_object()) out.push_back(ParseFunction(f));
            } else if (j.is_object()) {
                out.push_back(ParseFunction(j));
            }
            return out;
        }

        struct Trade {
            std::string id;
            Cost wants;
            std::optional<Cost> additionalWants;
            ItemID gives = Items::Air;
            int givesCount = 1;
            DataComponentMap givesComponents;
            Number maxUses = Number::Constant(4.0f);           // VillagerTrade.CODEC defaults
            Number xp = Number::Constant(1.0f);
            Number reputationDiscount = Number::Constant(0.0f);
            // merchant_predicate: entity_properties → villager/variant.
            bool hasPredicate = false;
            bool predicateUnderstood = false;
            std::vector<VillagerType> allowedTypes;
            std::vector<Function> modifiers;
            bool hasDoublePrice = false;
            std::vector<EnchantmentId> doublePrice;
        };

        struct TradeSet {
            std::vector<std::shared_ptr<Trade>> trades;
            Number amount;
            bool allowDuplicates = false;
        };

        bool ParseCost(const nlohmann::json& j, Cost& out) {
            if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return false;
            out.item = ItemFromId(j["id"].get<std::string>());
            if (out.item == Items::Air) return false;
            out.count = j.contains("count") ? ParseNumber(j["count"], 1.0f) : Number::Constant(1.0f);
            if (j.contains("components")) out.components = ParseComponents(j["components"]);
            return true;
        }

        std::shared_ptr<Trade> ParseTrade(const std::string& id, const nlohmann::json& j) {
            auto t = std::make_shared<Trade>();
            t->id = id;
            if (!j.contains("wants") || !ParseCost(j["wants"], t->wants)) {
                Log::Warning("[VillagerTrades] %s: missing or unknown \"wants\"", id.c_str());
                return nullptr;
            }
            if (j.contains("additional_wants")) {
                Cost extra;
                if (!ParseCost(j["additional_wants"], extra)) {
                    Log::Warning("[VillagerTrades] %s: unknown \"additional_wants\"", id.c_str());
                    return nullptr;
                }
                t->additionalWants = std::move(extra);
            }
            if (!j.contains("gives") || !j["gives"].is_object()) return nullptr;
            const auto& gives = j["gives"];
            t->gives = ItemFromId(gives.value("id", std::string()));
            if (t->gives == Items::Air) {
                Log::Warning("[VillagerTrades] %s: unknown \"gives\" item", id.c_str());
                return nullptr;
            }
            t->givesCount = gives.contains("count") && gives["count"].is_number()
                                ? static_cast<int>(gives["count"].get<double>()) : 1;
            if (gives.contains("components")) t->givesComponents = ParseComponents(gives["components"]);
            if (j.contains("max_uses")) t->maxUses = ParseNumber(j["max_uses"], 4.0f);
            if (j.contains("xp")) t->xp = ParseNumber(j["xp"], 1.0f);
            if (j.contains("reputation_discount")) t->reputationDiscount = ParseNumber(j["reputation_discount"], 0.0f);
            if (j.contains("merchant_predicate")) {
                t->hasPredicate = true;
                const auto& p = j["merchant_predicate"];
                if (p.is_object() && StripNs(p.value("condition", std::string())) == "entity_properties" &&
                    p.contains("predicate") && p["predicate"].is_object() &&
                    p["predicate"].contains("predicates") && p["predicate"]["predicates"].is_object()) {
                    const auto& preds = p["predicate"]["predicates"];
                    auto variant = preds.find("minecraft:villager/variant");
                    if (variant != preds.end()) {
                        t->predicateUnderstood = true;
                        for (const std::string& v : ReadStringOrList(*variant)) {
                            VillagerType type;
                            if (ParseVillagerType(v, type)) t->allowedTypes.push_back(type);
                        }
                    }
                }
                if (!t->predicateUnderstood) {
                    Log::Warning("[VillagerTrades] %s: merchant_predicate is not a villager/variant test; "
                                 "the trade is never offered", id.c_str());
                }
            }
            // The data files name the list "given_item_modifiers"; the codec
            // field is "given_item_modifier" (a function or a list). Both.
            if (j.contains("given_item_modifiers")) t->modifiers = ParseFunctions(j["given_item_modifiers"]);
            else if (j.contains("given_item_modifier")) t->modifiers = ParseFunctions(j["given_item_modifier"]);
            if (j.contains("double_trade_price_enchantments")) {
                t->hasDoublePrice = true;
                t->doublePrice = EnchantmentDefinitions::ResolveSet(
                    ReadStringOrList(j["double_trade_price_enchantments"]));
            }
            return t;
        }

        // ── Cache ─────────────────────────────────────────────────────────

        std::mutex s_mutex;
        std::unordered_map<std::string, std::shared_ptr<Trade>>    s_trades;     // by trade id
        std::unordered_map<std::string, std::unique_ptr<TradeSet>> s_sets;       // by set key (null = missing)

        std::shared_ptr<Trade> LookupTrade(const std::string& rawId) {
            const std::string id = rawId.find(':') == std::string::npos ? "minecraft:" + rawId : rawId;
            auto it = s_trades.find(id);
            if (it != s_trades.end()) return it->second;
            const auto [ns, path] = SplitId(id);
            nlohmann::json j;
            std::shared_ptr<Trade> t;
            if (ReadJson(DataRoot() / ns / "villager_trade" / (path + ".json"), j)) t = ParseTrade(id, j);
            else Log::Warning("[VillagerTrades] missing villager_trade %s", id.c_str());
            s_trades[id] = t;
            return t;
        }

        // A villager_trade HolderSet: "#ns:tag" (tags/villager_trade), an id,
        // or a list of either, in declaration order (MC's HolderSet order —
        // the draw indexes into it).
        void ResolveTrades(const std::vector<std::string>& entries,
                           std::vector<std::shared_ptr<Trade>>& out, std::set<std::string>& visiting) {
            for (const std::string& e : entries) {
                if (!e.empty() && e[0] == '#') {
                    const std::string tag = e.substr(1);
                    if (!visiting.insert(tag).second) continue;
                    const auto [ns, path] = SplitId(tag);
                    nlohmann::json j;
                    if (!ReadJson(DataRoot() / ns / "tags" / "villager_trade" / (path + ".json"), j)) {
                        Log::Warning("[VillagerTrades] missing villager_trade tag %s", tag.c_str());
                        continue;
                    }
                    std::vector<std::string> values;
                    if (j.contains("values") && j["values"].is_array()) {
                        for (const auto& v : j["values"]) {
                            if (v.is_string()) values.push_back(v.get<std::string>());
                            else if (v.is_object() && v.contains("id")) values.push_back(v["id"].get<std::string>());
                        }
                    }
                    ResolveTrades(values, out, visiting);
                    continue;
                }
                if (auto t = LookupTrade(e)) out.push_back(std::move(t));
            }
        }

        const TradeSet* LookupSet(const std::string& rawKey) {
            const std::string key = rawKey.find(':') == std::string::npos ? "minecraft:" + rawKey : rawKey;
            auto it = s_sets.find(key);
            if (it != s_sets.end()) return it->second.get();
            const auto [ns, path] = SplitId(key);
            nlohmann::json j;
            std::unique_ptr<TradeSet> set;
            if (ReadJson(DataRoot() / ns / "trade_set" / (path + ".json"), j)) {
                set = std::make_unique<TradeSet>();
                set->amount = j.contains("amount") ? ParseNumber(j["amount"], 0.0f) : Number::Constant(0.0f);
                set->allowDuplicates = j.value("allow_duplicates", false);
                std::set<std::string> visiting;
                if (j.contains("trades")) ResolveTrades(ReadStringOrList(j["trades"]), set->trades, visiting);
            } else {
                // MC LOGGER.debug("Missing expected trade set").
                Log::Debug("[VillagerTrades] missing trade set %s", key.c_str());
            }
            const TradeSet* raw = set.get();
            s_sets[key] = std::move(set);
            return raw;
        }

        // ── Applying a trade ───────────────────────────────────────────────

        struct OfferContext {
            JavaRandom& random;
            std::optional<VillagerType> merchantType;
            int additionalCost = 0;   // MC's ADDITIONAL_TRADE_COST component, carried aside
        };

        bool HasStoredEnchantments(const ItemStack& s) {
            const auto stored = s.get(DataComponents::STORED_ENCHANTMENTS);
            return stored && !stored->entries.empty();
        }

        // MC's DyeColor.textureDiffuseColor, ordinal order.
        constexpr int32_t kDyeDiffuse[16] = {
            16383998, 16351261, 13061821, 3847130, 16701501, 8439583, 15961002, 4673362,
            10329495, 1481884, 8991416, 3949738, 8606770, 6192150, 11546150, 1908001,
        };

        void ApplyAll(const std::vector<Function>& fns, ItemStack& stack, OfferContext& ctx);

        void Apply(const Function& fn, ItemStack& stack, OfferContext& ctx) {
            switch (fn.kind) {
                case Function::Kind::EnchantRandomly: {
                    // MC EnchantRandomlyFunction.run + enchantItem.
                    const bool targetIsBook = stack.itemId == Items::Book;
                    const bool checkCompat  = !targetIsBook && fn.onlyCompatible;
                    const std::vector<EnchantmentId>& source =
                        fn.hasOptions ? fn.options : EnchantmentDefinitions::All();
                    std::vector<EnchantmentId> compatible;
                    for (EnchantmentId id : source) {
                        if (!checkCompat || EnchantmentDefinitions::IsSupportedItem(id, stack.itemId)) {
                            compatible.push_back(id);
                        }
                    }
                    if (compatible.empty()) break;   // "Couldn't find a compatible enchantment"
                    const EnchantmentId id = compatible[static_cast<size_t>(
                        ctx.random.NextInt(static_cast<int>(compatible.size())))];
                    const int maxLevel = std::max(1, EnchantmentDefinitions::Get(id).maxLevel);
                    const int level = ctx.random.NextInt(1, maxLevel);   // Mth.nextInt(min 1, max)
                    if (targetIsBook) stack = ItemStack(Items::EnchantedBook, stack.count);
                    EnchantmentHelper::Enchant(stack, id, level);
                    if (fn.includeAdditionalCost) {
                        // 2 + nextInt(5 + level * 10) + 3 * level.
                        ctx.additionalCost = 2 + ctx.random.NextInt(5 + level * 10) + 3 * level;
                    }
                    break;
                }
                case Function::Kind::EnchantWithLevels: {
                    // MC EnchantWithLevelsFunction.run.
                    const int cost = fn.levels.GetInt(ctx.random);
                    stack = EnchantmentHelper::EnchantItem(
                        ctx.random, stack, cost, fn.hasOptions ? fn.options : EnchantmentDefinitions::All());
                    if (fn.includeAdditionalCost && !stack.IsEmpty() && cost > 0) ctx.additionalCost = cost;
                    break;
                }
                case Function::Kind::Filtered: {
                    if (fn.name == "sequence") { ApplyAll(fn.onPass, stack, ctx); break; }
                    // MC FilteredFunction: the item predicate, then on_pass /
                    // on_fail (absent = unchanged).
                    bool pass = fn.filterItems.empty() ||
                                std::find(fn.filterItems.begin(), fn.filterItems.end(), stack.itemId) !=
                                    fn.filterItems.end();
                    // minecraft:enchantments — the ENCHANTMENTS component,
                    // which gear does not carry in this engine: never met.
                    if (pass && fn.needsEnchantments) pass = false;
                    if (pass && fn.needsStoredEnchantments) pass = HasStoredEnchantments(stack);
                    if (pass && fn.needsDyedColor) pass = stack.get(DataComponents::DYED_COLOR).has_value();
                    // minecraft:map_id — no filled-map system: never met.
                    if (pass && fn.needsMapId) pass = false;
                    ApplyAll(pass ? fn.onPass : fn.onFail, stack, ctx);
                    break;
                }
                case Function::Kind::Discard:
                    stack.Clear();
                    break;
                case Function::Kind::SetName:
                    if (fn.text.empty()) break;
                    if (fn.itemName) stack.components.set(DataComponents::ITEM_NAME, fn.text);
                    else             stack.components.set(DataComponents::CUSTOM_NAME, fn.text);
                    break;
                case Function::Kind::SetPotion:
                    stack.components.set(DataComponents::POTION_CONTENTS,
                                         GetPotionContents(stack).WithPotion(fn.potion));
                    break;
                case Function::Kind::SetRandomPotion: {
                    // MC SetRandomPotionFunction.run: a random element of the
                    // options, else of the whole registry.
                    PotionId chosen = PotionId::Water;
                    bool have = false;
                    if (fn.hasPotionOptions) {
                        if (!fn.potions.empty()) {
                            chosen = fn.potions[static_cast<size_t>(
                                ctx.random.NextInt(static_cast<int>(fn.potions.size())))];
                            have = true;
                        }
                    } else if (kPotionCount > 0) {
                        chosen = static_cast<PotionId>(ctx.random.NextInt(kPotionCount));
                        have = true;
                    }
                    if (have) {
                        stack.components.set(DataComponents::POTION_CONTENTS,
                                             GetPotionContents(stack).WithPotion(chosen));
                    }
                    break;
                }
                case Function::Kind::SetStewEffect: {
                    // MC SetStewEffectFunction.run (see ChestLootTables).
                    if (stack.itemId != Items::SuspiciousStew || fn.stewEffects.empty()) break;
                    const auto& entry = fn.stewEffects[static_cast<size_t>(
                        ctx.random.NextInt(static_cast<int>(fn.stewEffects.size())))];
                    int duration = entry.second.GetInt(ctx.random);
                    if (!IsInstantenousEffect(entry.first)) duration *= 20;
                    SuspiciousStewEffects effects =
                        stack.get(DataComponents::SUSPICIOUS_STEW_EFFECTS).value_or(SuspiciousStewEffects{});
                    effects.effects.push_back({ entry.first, duration });
                    stack.components.set(DataComponents::SUSPICIOUS_STEW_EFFECTS, effects);
                    break;
                }
                case Function::Kind::SetRandomDyes: {
                    // MC SetRandomDyesFunction.run → DyedItemColor.applyDyes
                    // over that many random DyeColors (with the item's own
                    // dye, if any, in the average).
                    const int rolls = fn.dyes.GetInt(ctx.random);
                    if (rolls <= 0) break;
                    int redTotal = 0, greenTotal = 0, blueTotal = 0, intensityTotal = 0, colorCount = 0;
                    if (auto current = stack.get(DataComponents::DYED_COLOR)) {
                        const int r = (*current >> 16) & 0xFF, g = (*current >> 8) & 0xFF, b = *current & 0xFF;
                        intensityTotal += std::max(r, std::max(g, b));
                        redTotal += r; greenTotal += g; blueTotal += b;
                        ++colorCount;
                    }
                    for (int i = 0; i < rolls; ++i) {
                        const int32_t c = kDyeDiffuse[ctx.random.NextInt(16)];
                        const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
                        intensityTotal += std::max(r, std::max(g, b));
                        redTotal += r; greenTotal += g; blueTotal += b;
                        ++colorCount;
                    }
                    int red = redTotal / colorCount, green = greenTotal / colorCount, blue = blueTotal / colorCount;
                    const float averageIntensity = static_cast<float>(intensityTotal) / static_cast<float>(colorCount);
                    const float resultIntensity = static_cast<float>(std::max(red, std::max(green, blue)));
                    if (resultIntensity > 0.0f) {
                        red   = static_cast<int>(static_cast<float>(red)   * averageIntensity / resultIntensity);
                        green = static_cast<int>(static_cast<float>(green) * averageIntensity / resultIntensity);
                        blue  = static_cast<int>(static_cast<float>(blue)  * averageIntensity / resultIntensity);
                    }
                    // applyDyes returns copyWithCount(1) with the colour set.
                    stack.count = 1;
                    stack.components.set(DataComponents::DYED_COLOR,
                                         static_cast<int32_t>(((red & 0xFF) << 16) | ((green & 0xFF) << 8) | (blue & 0xFF)));
                    break;
                }
                case Function::Kind::ExplorationMap:
                    // MC ExplorationMapFunction: needs a filled-map system to
                    // write the found structure into. Without one the blank
                    // map is returned — MC's own result when no structure is
                    // found — and the trade's map_id filter discards it.
                    break;
                case Function::Kind::Unsupported:
                    Log::Debug("[VillagerTrades] item modifier '%s' is not supported; left unchanged",
                               fn.name.c_str());
                    break;
            }
        }

        void ApplyAll(const std::vector<Function>& fns, ItemStack& stack, OfferContext& ctx) {
            for (const Function& fn : fns) {
                if (stack.IsEmpty()) return;
                Apply(fn, stack, ctx);
            }
        }

        // MC TradeCost.toItemCost: clamp(count + additionalCost, 0,
        // item.getDefaultMaxStackSize()).
        ItemCost ToItemCost(const Cost& cost, JavaRandom& random, int additionalCost) {
            ItemCost c;
            c.item = cost.item;
            const int maxStack = ItemRegistry::Get(cost.item).maxStackSize;
            c.count = std::clamp(cost.count.GetInt(random) + additionalCost, 0, maxStack);
            c.components = cost.components;
            return c;
        }

        // MC VillagerTrade.getOffer.
        std::optional<MerchantOffer> GetOffer(const Trade& t, OfferContext& ctx) {
            if (t.hasPredicate) {
                if (!t.predicateUnderstood || !ctx.merchantType) return std::nullopt;
                if (std::find(t.allowedTypes.begin(), t.allowedTypes.end(), *ctx.merchantType) ==
                    t.allowedTypes.end()) {
                    return std::nullopt;
                }
            }
            ItemStack result(t.gives, t.givesCount);
            result.components = t.givesComponents;
            ctx.additionalCost = 0;
            if (!t.modifiers.empty()) {
                ApplyAll(t.modifiers, result, ctx);
                if (result.IsEmpty()) return std::nullopt;
            }
            int additionalCost = ctx.additionalCost;
            if (t.hasDoublePrice) {
                if (auto stored = result.get(DataComponents::STORED_ENCHANTMENTS)) {
                    for (const auto& e : stored->entries) {
                        if (std::find(t.doublePrice.begin(), t.doublePrice.end(),
                                      static_cast<EnchantmentId>(e.id)) != t.doublePrice.end()) {
                            additionalCost *= 2;
                            break;
                        }
                    }
                }
            }
            ItemCost costA = ToItemCost(t.wants, ctx.random, additionalCost);
            if (costA.count < 1) return std::nullopt;
            std::optional<ItemCost> costB;
            if (t.additionalWants) {
                costB = ToItemCost(*t.additionalWants, ctx.random, 0);
                if (costB->count < 1) return std::nullopt;
            }
            const int maxUses = std::max(t.maxUses.GetInt(ctx.random), 1);
            const int xp = std::max(t.xp.GetInt(ctx.random), 0);
            const float discount = std::max(t.reputationDiscount.GetFloat(ctx.random), 0.0f);
            return MerchantOffer(std::move(costA), std::move(costB), std::move(result),
                                 0, maxUses, xp, discount);
        }

    } // namespace

    void AddOffersFromTradeSet(const std::string& tradeSetKey, MerchantOffers& offers,
                               JavaRandom& random, std::optional<VillagerType> merchantType) {
        std::vector<std::shared_ptr<Trade>> potential;
        int numberOfOffers = 0;
        bool allowDuplicates = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            const TradeSet* set = LookupSet(tradeSetKey);
            if (!set) return;
            potential = set->trades;
            numberOfOffers = set->amount.GetInt(random);
            allowDuplicates = set->allowDuplicates;
        }
        OfferContext ctx{ random, merchantType, 0 };
        int found = 0;
        if (allowDuplicates) {
            // MC addOffersFromItemListings: draw with replacement; a trade
            // that declines is struck from the list.
            while (found < numberOfOffers && !potential.empty()) {
                const int roll = random.NextInt(static_cast<int>(potential.size()));
                auto offer = GetOffer(*potential[static_cast<size_t>(roll)], ctx);
                if (!offer) {
                    potential.erase(potential.begin() + roll);
                } else {
                    offers.push_back(std::move(*offer));
                    ++found;
                }
            }
        } else {
            // MC addOffersFromItemListingsWithoutDuplicates: every draw is
            // removed, offered or not.
            while (found < numberOfOffers && !potential.empty()) {
                const int roll = random.NextInt(static_cast<int>(potential.size()));
                const std::shared_ptr<Trade> t = potential[static_cast<size_t>(roll)];
                potential.erase(potential.begin() + roll);
                if (auto offer = GetOffer(*t, ctx)) {
                    offers.push_back(std::move(*offer));
                    ++found;
                }
            }
        }
    }

    bool TradeSetExists(const std::string& tradeSetKey) {
        std::lock_guard<std::mutex> lock(s_mutex);
        return LookupSet(tradeSetKey) != nullptr;
    }

    void Reload() {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_trades.clear();
        s_sets.clear();
    }

} // namespace Game::VillagerTrades
