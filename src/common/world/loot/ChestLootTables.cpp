// File: src/common/world/loot/ChestLootTables.cpp
#include "ChestLootTables.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Instruments.hpp"
#include "common/entity/Item.hpp"
#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/map/MapItem.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace Game::ChestLoot {

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

        std::string StripNamespace(const std::string& id) {
            const size_t colon = id.find(':');
            return colon == std::string::npos ? id : id.substr(colon + 1);
        }

        // A HolderSet in JSON: one id / "#tag", or a list of ids.
        std::vector<std::string> ReadStringOrListOf(const nlohmann::json& j) {
            std::vector<std::string> out;
            if (j.is_string()) out.push_back(j.get<std::string>());
            else if (j.is_array()) for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
            return out;
        }

        // ── Parsed representation ────────────────────────────────────────

        // MC NumberProvider: constant / uniform / binomial (the three the
        // data pack uses in loot tables).
        struct NumberProvider {
            enum class Kind : uint8_t { Constant, Uniform, Binomial };
            Kind  kind = Kind::Constant;
            float a = 0.0f;   // constant value | uniform min | binomial n
            float b = 0.0f;   // uniform max | binomial p

            // MC UniformGenerator.getInt: Mth.nextInt(random, floor(min), floor(max)),
            // inclusive both ends. BinomialDistributionGenerator: n trials at p.
            int GetInt(JavaRandom& random) const {
                switch (kind) {
                    case Kind::Constant: return static_cast<int>(std::floor(a));
                    case Kind::Uniform: {
                        const int lo = static_cast<int>(std::floor(a));
                        const int hi = static_cast<int>(std::floor(b));
                        return hi <= lo ? lo : random.NextInt(lo, hi);
                    }
                    case Kind::Binomial: {
                        int n = static_cast<int>(a), value = 0;
                        for (int i = 0; i < n; ++i) if (random.NextFloat() < b) ++value;
                        return value;
                    }
                }
                return 0;
            }
            float GetFloat(JavaRandom& random) const {
                switch (kind) {
                    case Kind::Constant: return a;
                    case Kind::Uniform:  return a + random.NextFloat() * (b - a);
                    case Kind::Binomial: return static_cast<float>(GetInt(random));
                }
                return 0.0f;
            }
        };

        struct Condition {
            enum class Kind : uint8_t { RandomChance, Inverted, AllOf, AnyOf, LocationCheck, FishingHookOpenWater, Unsupported };
            Kind  kind = Kind::Unsupported;
            float chance = 1.0f;
            std::string name;
            std::vector<Condition> terms;       // inverted (one) / all_of / any_of
            // location_check: LocationPredicate.biomes (ids or "#tags") at
            // ORIGIN + offset.
            std::vector<std::string> biomes;
            glm::ivec3 offset{0};
            // entity_properties {entity: this, predicate: {type_specific:
            // {type: fishing_hook, in_open_water}}}: the required value.
            bool hasInOpenWater = false;
            bool inOpenWater = true;
        };

        // MC ListOperation (loot/functions/ListOperation.java) — how a
        // set_*_book_pages function combines its pages with the book's.
        struct ListOperation {
            enum class Mode : uint8_t { ReplaceAll, ReplaceSection, Insert, Append };
            Mode mode = Mode::ReplaceAll;
            int  offset = 0;                   // replace_section / insert
            std::optional<int> size;           // replace_section

            // MC ListOperation.apply(original, replacement, maxSize). Every
            // failure logs and returns the original list, as vanilla does.
            template <class T>
            std::vector<T> Apply(const std::vector<T>& original, const std::vector<T>& replacement,
                                 size_t maxSize, const std::string& tableKey) const {
                const size_t originalSize = original.size();
                switch (mode) {
                    case Mode::ReplaceAll:
                        return replacement;
                    case Mode::ReplaceSection: {
                        if (static_cast<size_t>(offset) > originalSize) {
                            Log::Error("[ChestLoot] %s: Cannot replace when offset is out of bounds", tableKey.c_str());
                            return original;
                        }
                        std::vector<T> out(original.begin(), original.begin() + offset);
                        out.insert(out.end(), replacement.begin(), replacement.end());
                        const size_t resume = static_cast<size_t>(offset) +
                            (size ? static_cast<size_t>(*size) : replacement.size());
                        if (resume < originalSize) out.insert(out.end(), original.begin() + resume, original.end());
                        if (out.size() > maxSize) {
                            Log::Error("[ChestLoot] %s: Contents overflow in section replacement", tableKey.c_str());
                            return original;
                        }
                        return out;
                    }
                    case Mode::Insert: {
                        if (static_cast<size_t>(offset) > originalSize) {
                            Log::Error("[ChestLoot] %s: Cannot insert when offset is out of bounds", tableKey.c_str());
                            return original;
                        }
                        if (originalSize + replacement.size() > maxSize) {
                            Log::Error("[ChestLoot] %s: Contents overflow in section insertion", tableKey.c_str());
                            return original;
                        }
                        std::vector<T> out(original.begin(), original.begin() + offset);
                        out.insert(out.end(), replacement.begin(), replacement.end());
                        out.insert(out.end(), original.begin() + offset, original.end());
                        return out;
                    }
                    case Mode::Append: {
                        if (originalSize + replacement.size() > maxSize) {
                            Log::Error("[ChestLoot] %s: Contents overflow in section append", tableKey.c_str());
                            return original;
                        }
                        std::vector<T> out = original;
                        out.insert(out.end(), replacement.begin(), replacement.end());
                        return out;
                    }
                }
                return original;
            }
        };

        struct Function {
            enum class Kind : uint8_t {
                SetCount, SetName, EnchantRandomly, EnchantWithLevels, SetEnchantments,
                SetPotion, SetStewEffect, SetWrittenBookPages, SetBookCover, SetWritableBookPages,
                SetDamage, ExplorationMap, Filtered, Discard,
                SetOminousBottleAmplifier, SetComponents, SetInstrument, Unsupported
            };
            Kind kind = Kind::Unsupported;
            std::string name;                  // the JSON "function" id, for logging
            std::vector<Condition> conditions;
            NumberProvider count;              // set_count / enchant_with_levels levels / set_damage damage
            bool add = false;                  // set_count / set_damage add
            std::string text;                  // set_name
            bool itemName = false;             // set_name target "item_name" (else custom_name)
            ExplorationMapParams exploration;  // exploration_map
            // filtered: ItemPredicate {items?, predicates: {map_id?}} and the
            // functions to run on a pass / a fail.
            std::vector<ItemID> filterItems;
            bool filterNeedsMapId = false;
            std::vector<Function> onPass, onFail;
            bool hasOptions = false;           // enchant_*: options present
            std::vector<EnchantmentId> options;
            bool onlyCompatible = true;        // enchant_randomly
            std::vector<std::pair<EnchantmentId, NumberProvider>> enchantments;   // set_enchantments
            PotionId potion = PotionId::Water;                                    // set_potion
            std::vector<std::pair<MobEffectId, NumberProvider>> stewEffects;      // set_stew_effect
            ListOperation pageOperation;                                          // set_*_book_pages
            std::vector<Filterable<Text::Component>> writtenPages;                // set_written_book_pages
            std::vector<Filterable<std::string>>     writablePages;               // set_writable_book_pages
            std::optional<Filterable<std::string>>   coverTitle;                  // set_book_cover
            std::optional<std::string>               coverAuthor;
            std::optional<int>                       coverGeneration;
            // set_components: the patch, component id -> its JSON value, in
            // file order (applyComponentsAndValidate applies them in turn).
            std::vector<std::pair<std::string, nlohmann::json>> components;
            // set_instrument: the `options` HolderSet ("#minecraft:
            // regular_goat_horns" or one id), resolved at parse time.
            std::vector<std::string> instrumentOptions;
        };

        struct Table;

        struct Entry {
            enum class Kind : uint8_t { Item, Empty, LootTableRef, Unsupported };
            Kind kind = Kind::Unsupported;
            ItemID item = Items::Air;
            std::string tableRef;              // loot_table entry: the key
            // MC NestedLootTable.value is a HolderSet<LootTable>: besides one
            // key, a list of keys and inline tables — every table in it is
            // rolled in turn (the equipment tables nest theirs inline).
            std::vector<std::string> moreTableRefs;
            std::vector<std::shared_ptr<Table>> inlineTables;
            int weight = 1;
            int quality = 0;
            std::vector<Condition> conditions;
            std::vector<Function> functions;
        };

        struct Pool {
            NumberProvider rolls;
            NumberProvider bonusRolls;
            std::vector<Condition> conditions;
            std::vector<Function> functions;
            std::vector<Entry> entries;
        };

        struct Table {
            std::string key;
            std::vector<Pool> pools;
            std::vector<Function> functions;
            bool warnedUnsupported = false;
        };

        // ── Parsing ──────────────────────────────────────────────────────

        NumberProvider ParseNumber(const nlohmann::json& j, float fallback) {
            NumberProvider p;
            if (j.is_number()) { p.kind = NumberProvider::Kind::Constant; p.a = j.get<float>(); return p; }
            if (!j.is_object()) { p.kind = NumberProvider::Kind::Constant; p.a = fallback; return p; }
            std::string type = j.value("type", std::string("minecraft:uniform"));
            type = StripNamespace(type);
            auto number = [&](const char* key, float def) {
                if (!j.contains(key)) return def;
                return ParseNumber(j[key], def).a;   // nested providers: constant part only
            };
            if (type == "constant") {
                p.kind = NumberProvider::Kind::Constant;
                p.a = number("value", fallback);
            } else if (type == "binomial") {
                p.kind = NumberProvider::Kind::Binomial;
                p.a = number("n", 0.0f);
                p.b = number("p", 0.0f);
            } else {
                p.kind = NumberProvider::Kind::Uniform;
                p.a = number("min", 0.0f);
                p.b = number("max", 0.0f);
            }
            return p;
        }

        std::vector<Condition> ParseConditions(const nlohmann::json& j);

        Condition ParseCondition(const nlohmann::json& c) {
            Condition cond;
            if (!c.is_object()) return cond;
            cond.name = StripNamespace(c.value("condition", std::string()));
            if (cond.name == "random_chance") {
                cond.kind = Condition::Kind::RandomChance;
                cond.chance = c.contains("chance") ? ParseNumber(c["chance"], 1.0f).a : 1.0f;
            } else if (cond.name == "inverted" && c.contains("term")) {
                // InvertedLootItemCondition {term}.
                cond.kind = Condition::Kind::Inverted;
                cond.terms.push_back(ParseCondition(c["term"]));
            } else if ((cond.name == "all_of" || cond.name == "any_of") && c.contains("terms")) {
                cond.kind = cond.name == "all_of" ? Condition::Kind::AllOf : Condition::Kind::AnyOf;
                cond.terms = ParseConditions(c["terms"]);
            } else if (cond.name == "entity_properties") {
                // LootItemEntityPropertyCondition {entity, predicate}. The
                // one shape the data pack uses outside entity drops: the
                // fishing table's treasure gate on the hook itself —
                // EntityPredicate {type_specific: FishingHookPredicate
                // {in_open_water}}. Any other predicate stays Unsupported.
                const std::string entity = c.value("entity", std::string());
                const nlohmann::json predicate = c.value("predicate", nlohmann::json::object());
                if (entity == "this" && predicate.is_object() && predicate.size() == 1 &&
                    predicate.contains("type_specific") && predicate["type_specific"].is_object()) {
                    const nlohmann::json& ts = predicate["type_specific"];
                    if (StripNamespace(ts.value("type", std::string())) == "fishing_hook") {
                        cond.kind = Condition::Kind::FishingHookOpenWater;
                        // FishingHookPredicate.inOpenWater is Optional<Boolean>:
                        // absent matches any hook.
                        if (ts.contains("in_open_water") && ts["in_open_water"].is_boolean()) {
                            cond.hasInOpenWater = true;
                            cond.inOpenWater = ts["in_open_water"].get<bool>();
                        }
                    }
                }
            } else if (cond.name == "location_check") {
                // LocationCheck {predicate: LocationPredicate, offsetX/Y/Z};
                // the biome clause is the one the data pack uses.
                const nlohmann::json predicate = c.value("predicate", nlohmann::json::object());
                if (predicate.is_object() && predicate.contains("biomes") && predicate.size() == 1) {
                    cond.kind = Condition::Kind::LocationCheck;
                    cond.biomes = ReadStringOrListOf(predicate["biomes"]);
                    cond.offset = glm::ivec3(c.value("offsetX", 0), c.value("offsetY", 0), c.value("offsetZ", 0));
                }
            }
            return cond;
        }

        std::vector<Condition> ParseConditions(const nlohmann::json& j) {
            std::vector<Condition> out;
            if (j.is_object()) {
                out.push_back(ParseCondition(j));
            } else if (j.is_array()) {
                for (const auto& c : j) out.push_back(ParseCondition(c));
            }
            return out;
        }

        // MC's Component JSON as loot tables write it: a plain string, or an
        // object with "text" / "translate". There is no translation lookup
        // for arbitrary keys here, so a translate key falls back to its
        // last segment ("filled_map.buried_treasure" → "buried_treasure").
        std::string ParseText(const nlohmann::json& j) {
            if (j.is_string()) return j.get<std::string>();
            if (j.is_object()) {
                if (j.contains("text") && j["text"].is_string()) return j["text"].get<std::string>();
                if (j.contains("translate") && j["translate"].is_string()) {
                    // The key through the language table ("filled_map.
                    // buried_treasure" → "Buried Treasure Map"); a key the
                    // table lacks falls back to its last segment.
                    std::string key = j["translate"].get<std::string>();
                    const std::string translated = Language::Get(key);
                    if (!translated.empty() && translated != key) return translated;
                    const size_t dot = key.rfind('.');
                    if (dot != std::string::npos) key = key.substr(dot + 1);
                    std::replace(key.begin(), key.end(), '_', ' ');
                    return key;
                }
            }
            return {};
        }

        std::vector<std::string> ReadSet(const nlohmann::json& j) {
            std::vector<std::string> out;
            if (j.is_string()) out.push_back(j.get<std::string>());
            else if (j.is_array()) for (const auto& e : j) if (e.is_string()) out.push_back(e.get<std::string>());
            return out;
        }

        // Codec.string(min, max) bounds UTF-16 code units; a code point past
        // U+FFFF is two of them.
        size_t Utf16Length(const std::string& s) {
            size_t n = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if ((c & 0xC0) == 0x80) continue;          // continuation byte
                n += (c >= 0xF0) ? 2 : 1;
            }
            return n;
        }

        // MC Filterable.codec(valueCodec): the full {raw, filtered?} form, or
        // a bare value (Codec.withAlternative). `parse` reads one value.
        template <class T, class Parse>
        std::optional<Filterable<T>> ParseFilterable(const nlohmann::json& j, Parse parse) {
            if (j.is_object() && j.contains("raw")) {
                auto raw = parse(j["raw"]);
                if (!raw) return std::nullopt;
                Filterable<T> out{std::move(*raw), std::nullopt};
                if (j.contains("filtered")) {
                    auto filtered = parse(j["filtered"]);
                    if (!filtered) return std::nullopt;
                    out.filtered = std::move(*filtered);
                }
                return out;
            }
            auto value = parse(j);
            if (!value) return std::nullopt;
            return Filterable<T>::PassThrough(std::move(*value));
        }

        // WrittenBookContent.CONTENT_CODEC = flatRestrictedCodec(32767): a
        // component whose JSON encoding is no longer than a page may be.
        std::optional<Text::Component> ParsePageComponent(const nlohmann::json& j) {
            auto c = Text::FromJson(j);
            if (!c || Text::EncodesLongerThan(*c, WrittenBookContent::PAGE_LENGTH)) return std::nullopt;
            return c;
        }

        // Codec.string(0, maxLength).
        auto StringOfAtMost(size_t maxLength) {
            return [maxLength](const nlohmann::json& j) -> std::optional<std::string> {
                if (!j.is_string()) return std::nullopt;
                std::string s = j.get<std::string>();
                if (Utf16Length(s) > maxLength) return std::nullopt;
                return s;
            };
        }

        // MC ListOperation.codec(maxSize): dispatch on "mode"; replace_section's
        // size must not exceed maxSize. Offsets / sizes are NON_NEGATIVE_INT.
        bool ParseListOperation(const nlohmann::json& f, int maxSize, ListOperation& out, std::string& error) {
            if (!f.contains("mode") || !f["mode"].is_string()) {
                error = "missing \"mode\"";
                return false;
            }
            const std::string mode = f["mode"].get<std::string>();
            auto nonNegative = [&](const char* key, std::optional<int>& value) {
                if (!f.contains(key)) return true;
                if (!f[key].is_number_integer() || f[key].get<long long>() < 0 ||
                    f[key].get<long long>() > INT_MAX) {
                    error = std::string("\"") + key + "\" must be a non-negative int";
                    return false;
                }
                value = static_cast<int>(f[key].get<long long>());
                return true;
            };
            std::optional<int> offset;
            if (mode == "replace_all") {
                out.mode = ListOperation::Mode::ReplaceAll;
            } else if (mode == "append") {
                out.mode = ListOperation::Mode::Append;
            } else if (mode == "insert") {
                out.mode = ListOperation::Mode::Insert;
                if (!nonNegative("offset", offset)) return false;
                out.offset = offset.value_or(0);
            } else if (mode == "replace_section") {
                out.mode = ListOperation::Mode::ReplaceSection;
                if (!nonNegative("offset", offset) || !nonNegative("size", out.size)) return false;
                out.offset = offset.value_or(0);
                if (out.size && *out.size > maxSize) {
                    error = "Size value too large: " + std::to_string(*out.size) +
                            ", max size is " + std::to_string(maxSize);
                    return false;
                }
            } else {
                error = "unknown mode \"" + mode + "\"";
                return false;
            }
            return true;
        }

        std::vector<Function> ParseFunctions(const nlohmann::json& input) {
            std::vector<Function> out;
            nlohmann::json j = input;
            if (j.is_object()) j = nlohmann::json::array({input});   // a single function
            if (!j.is_array()) return out;
            for (const auto& f : j) {
                Function fn;
                fn.name = StripNamespace(f.value("function", std::string()));
                if (f.contains("conditions")) fn.conditions = ParseConditions(f["conditions"]);
                if (fn.name == "set_count") {
                    fn.kind  = Function::Kind::SetCount;
                    fn.count = f.contains("count") ? ParseNumber(f["count"], 1.0f) : NumberProvider{};
                    fn.add   = f.value("add", false);
                } else if (fn.name == "set_damage") {
                    // MC SetItemDamageFunction: {damage: NumberProvider (the
                    // durability FRACTION left), add}.
                    fn.kind  = Function::Kind::SetDamage;
                    fn.count = f.contains("damage") ? ParseNumber(f["damage"], 1.0f) : NumberProvider{};
                    fn.add   = f.value("add", false);
                } else if (fn.name == "set_name") {
                    // SetNameFunction {name, target: custom_name | item_name}.
                    fn.kind = Function::Kind::SetName;
                    if (f.contains("name")) fn.text = ParseText(f["name"]);
                    fn.itemName = StripNamespace(f.value("target", std::string("custom_name"))) == "item_name";
                } else if (fn.name == "exploration_map") {
                    // ExplorationMapFunction {destination, decoration?, zoom?,
                    // search_radius?, skip_existing_chunks?}.
                    fn.kind = Function::Kind::ExplorationMap;
                    fn.exploration.destination = f.value("destination", std::string("#minecraft:on_treasure_maps"));
                    if (f.contains("decoration") && f["decoration"].is_string()) {
                        if (auto type = Maps::DecorationTypeFromKey(f["decoration"].get<std::string>())) {
                            fn.exploration.decoration = *type;
                        } else {
                            Log::Warning("[ChestLoot] exploration_map: unknown decoration %s",
                                         f["decoration"].dump().c_str());
                        }
                    }
                    fn.exploration.zoom = f.value("zoom", 2);
                    fn.exploration.searchRadius = f.value("search_radius", 50);
                    fn.exploration.skipKnownStructures = f.value("skip_existing_chunks", true);
                } else if (fn.name == "filtered") {
                    // FilteredFunction {item_filter: ItemPredicate, on_pass?, on_fail?}.
                    fn.kind = Function::Kind::Filtered;
                    const nlohmann::json filter = f.value("item_filter", nlohmann::json::object());
                    if (filter.is_object()) {
                        if (filter.contains("items")) {
                            for (const std::string& id : ReadStringOrListOf(filter["items"])) {
                                if (id.empty() || id[0] == '#') continue;
                                const ItemID item = RecipeManager::ItemFromSlug(StripNamespace(id));
                                if (item != Items::Air) fn.filterItems.push_back(item);
                            }
                        }
                        if (filter.contains("predicates") && filter["predicates"].is_object()) {
                            for (auto it = filter["predicates"].begin(); it != filter["predicates"].end(); ++it) {
                                if (StripNamespace(it.key()) == "map_id") fn.filterNeedsMapId = true;
                                else Log::Warning("[ChestLoot] filtered: item predicate '%s' is not modelled",
                                                  it.key().c_str());
                            }
                        }
                    }
                    if (f.contains("on_pass")) fn.onPass = ParseFunctions(f["on_pass"]);
                    if (f.contains("on_fail")) fn.onFail = ParseFunctions(f["on_fail"]);
                } else if (fn.name == "discard") {
                    fn.kind = Function::Kind::Discard;
                } else if (fn.name == "enchant_randomly") {
                    fn.kind = Function::Kind::EnchantRandomly;
                    if (f.contains("options")) {
                        fn.hasOptions = true;
                        fn.options = EnchantmentDefinitions::ResolveSet(ReadSet(f["options"]));
                    }
                    fn.onlyCompatible = f.value("only_compatible", true);
                } else if (fn.name == "enchant_with_levels") {
                    fn.kind  = Function::Kind::EnchantWithLevels;
                    fn.count = f.contains("levels") ? ParseNumber(f["levels"], 1.0f) : NumberProvider{};
                    if (f.contains("options")) {
                        fn.hasOptions = true;
                        fn.options = EnchantmentDefinitions::ResolveSet(ReadSet(f["options"]));
                    }
                } else if (fn.name == "set_potion") {
                    // MC SetPotionFunction: {"id": "<potion>"}.
                    PotionId id;
                    if (ParsePotionId(f.value("id", std::string()), id)) {
                        fn.kind   = Function::Kind::SetPotion;
                        fn.potion = id;
                    }
                } else if (fn.name == "set_stew_effect") {
                    // MC SetStewEffectFunction: effects [{type, duration}].
                    fn.kind = Function::Kind::SetStewEffect;
                    if (f.contains("effects") && f["effects"].is_array()) {
                        for (const auto& e : f["effects"]) {
                            MobEffectId id;
                            if (!e.is_object() || !ParseEffectId(e.value("type", std::string()), id)) continue;
                            fn.stewEffects.emplace_back(
                                id, e.contains("duration") ? ParseNumber(e["duration"], 1.0f)
                                                           : NumberProvider{});
                        }
                    }
                } else if (fn.name == "set_written_book_pages" || fn.name == "set_writable_book_pages") {
                    // MC SetWrittenBookPagesFunction {pages: WrittenBookContent
                    // .PAGES_CODEC, ListOperation.UNLIMITED_CODEC} and
                    // SetWritableBookPagesFunction {pages: WritableBookContent
                    // .PAGES_CODEC (≤100 pages of ≤1024 chars), ListOperation
                    // .codec(100)}. A page the codec rejects fails the whole
                    // function in MC; here the function is dropped with a
                    // warning and the rest of the table still rolls.
                    const bool written = fn.name == "set_written_book_pages";
                    std::string error;
                    bool ok = f.contains("pages") && f["pages"].is_array();
                    if (!ok) error = "missing \"pages\" list";
                    if (ok && !written && f["pages"].size() > static_cast<size_t>(WritableBookContent::MAX_PAGES)) {
                        ok = false;
                        error = "more than 100 pages";
                    }
                    if (ok) {
                        for (const auto& page : f["pages"]) {
                            if (written) {
                                auto p = ParseFilterable<Text::Component>(page, &ParsePageComponent);
                                if (!p) { ok = false; error = "invalid page " + page.dump(); break; }
                                fn.writtenPages.push_back(std::move(*p));
                            } else {
                                auto p = ParseFilterable<std::string>(
                                    page, StringOfAtMost(WritableBookContent::PAGE_EDIT_LENGTH));
                                if (!p) { ok = false; error = "invalid page " + page.dump(); break; }
                                fn.writablePages.push_back(std::move(*p));
                            }
                        }
                    }
                    if (ok) ok = ParseListOperation(f, written ? INT_MAX : WritableBookContent::MAX_PAGES,
                                                    fn.pageOperation, error);
                    if (ok) {
                        fn.kind = written ? Function::Kind::SetWrittenBookPages
                                          : Function::Kind::SetWritableBookPages;
                    } else {
                        Log::Warning("[ChestLoot] %s: %s", fn.name.c_str(), error.c_str());
                    }
                } else if (fn.name == "set_book_cover") {
                    // MC SetBookCoverFunction {title?: Filterable<string(0,32)>,
                    // author?: string, generation?: int 0..3}.
                    bool ok = true;
                    if (f.contains("title")) {
                        fn.coverTitle = ParseFilterable<std::string>(
                            f["title"], StringOfAtMost(WrittenBookContent::TITLE_MAX_LENGTH));
                        ok = fn.coverTitle.has_value();
                    }
                    if (ok && f.contains("author")) {
                        ok = f["author"].is_string();
                        if (ok) fn.coverAuthor = f["author"].get<std::string>();
                    }
                    if (ok && f.contains("generation")) {
                        const auto& g = f["generation"];
                        ok = g.is_number_integer() && g.get<long long>() >= 0 &&
                             g.get<long long>() <= WrittenBookContent::MAX_GENERATION;
                        if (ok) fn.coverGeneration = static_cast<int>(g.get<long long>());
                    }
                    if (ok) fn.kind = Function::Kind::SetBookCover;
                    else Log::Warning("[ChestLoot] set_book_cover: invalid title, author or generation in %s",
                                      f.dump().c_str());
                } else if (fn.name == "set_ominous_bottle_amplifier") {
                    // SetOminousBottleAmplifierFunction {amplifier: int provider}.
                    fn.kind  = Function::Kind::SetOminousBottleAmplifier;
                    fn.count = f.contains("amplifier") ? ParseNumber(f["amplifier"], 0.0f) : NumberProvider{};
                } else if (fn.name == "set_instrument") {
                    // SetInstrumentFunction {options: TagKey<Instrument>}
                    // ("#minecraft:regular_goat_horns" - the pillager
                    // outpost's horn).
                    fn.kind = Function::Kind::SetInstrument;
                    std::string spec;
                    if (f.contains("options") && f["options"].is_string()) spec = f["options"].get<std::string>();
                    fn.instrumentOptions = Instruments::Resolve(spec);
                    if (fn.instrumentOptions.empty()) {
                        Log::Warning("[ChestLoot] set_instrument: no instrument in '%s'", spec.c_str());
                    }
                } else if (fn.name == "set_components") {
                    // SetComponentsFunction {components: DataComponentPatch}.
                    fn.kind = Function::Kind::SetComponents;
                    if (f.contains("components") && f["components"].is_object()) {
                        for (auto it = f["components"].begin(); it != f["components"].end(); ++it) {
                            fn.components.emplace_back(it.key(), it.value());
                        }
                    }
                } else if (fn.name == "set_enchantments") {
                    fn.kind = Function::Kind::SetEnchantments;
                    fn.add  = f.value("add", false);
                    if (f.contains("enchantments") && f["enchantments"].is_object()) {
                        for (auto it = f["enchantments"].begin(); it != f["enchantments"].end(); ++it) {
                            if (auto id = EnchantmentRegistry::ByName(StripNamespace(it.key()))) {
                                fn.enchantments.emplace_back(*id, ParseNumber(it.value(), 1.0f));
                            }
                        }
                    }
                }
                out.push_back(std::move(fn));
            }
            return out;
        }

        std::unique_ptr<Table> ParseTable(const std::string& key, const nlohmann::json& j);

        Entry ParseEntry(const nlohmann::json& j) {
            Entry e;
            const std::string type = StripNamespace(j.value("type", std::string()));
            e.weight  = j.value("weight", 1);
            e.quality = j.value("quality", 0);
            if (j.contains("conditions")) e.conditions = ParseConditions(j["conditions"]);
            if (j.contains("functions"))  e.functions  = ParseFunctions(j["functions"]);
            if (type == "item") {
                e.kind = Entry::Kind::Item;
                e.item = RecipeManager::ItemFromSlug(StripNamespace(j.value("name", std::string())));
                if (e.item == Items::Air) {
                    // An item this build lacks: MC would refuse the whole
                    // table; dropping the entry keeps the rest of the chest.
                    e.kind = Entry::Kind::Unsupported;
                }
            } else if (type == "empty") {
                e.kind = Entry::Kind::Empty;
            } else if (type == "loot_table") {
                e.kind = Entry::Kind::LootTableRef;
                const auto addOne = [&e](const nlohmann::json& v) {
                    if (v.is_string()) {
                        std::string key = WithNamespace(v.get<std::string>());
                        if (e.tableRef.empty()) e.tableRef = std::move(key);
                        else e.moreTableRefs.push_back(std::move(key));
                        return true;
                    }
                    if (v.is_object()) {
                        e.inlineTables.push_back(ParseTable("<inline>", v));
                        return true;
                    }
                    return false;
                };
                bool ok = j.contains("value");
                if (ok && j["value"].is_array()) {
                    for (const auto& v : j["value"]) ok = addOne(v) && ok;
                } else if (ok) {
                    ok = addOne(j["value"]);
                }
                if (!ok) e.kind = Entry::Kind::Unsupported;
            }
            return e;
        }

        std::unique_ptr<Table> ParseTable(const std::string& key, const nlohmann::json& j) {
            auto t = std::make_unique<Table>();
            t->key = key;
            if (j.contains("functions")) t->functions = ParseFunctions(j["functions"]);
            if (j.contains("pools") && j["pools"].is_array()) {
                for (const auto& p : j["pools"]) {
                    Pool pool;
                    pool.rolls      = p.contains("rolls") ? ParseNumber(p["rolls"], 1.0f) : NumberProvider{NumberProvider::Kind::Constant, 1.0f, 0.0f};
                    pool.bonusRolls = p.contains("bonus_rolls") ? ParseNumber(p["bonus_rolls"], 0.0f) : NumberProvider{};
                    if (p.contains("conditions")) pool.conditions = ParseConditions(p["conditions"]);
                    if (p.contains("functions"))  pool.functions  = ParseFunctions(p["functions"]);
                    if (p.contains("entries") && p["entries"].is_array()) {
                        for (const auto& e : p["entries"]) pool.entries.push_back(ParseEntry(e));
                    }
                    t->pools.push_back(std::move(pool));
                }
            }
            return t;
        }

        // ── Cache ────────────────────────────────────────────────────────

        std::mutex g_mutex;
        std::unordered_map<std::string, std::unique_ptr<Table>> g_tables;   // key → table (nullptr = missing)

        // "minecraft:chests/village/village_plains_house" → data/minecraft/loot_table/chests/village/village_plains_house.json
        std::filesystem::path PathFor(const std::string& key) {
            const size_t colon = key.find(':');
            const std::string ns   = colon == std::string::npos ? "minecraft" : key.substr(0, colon);
            const std::string path = colon == std::string::npos ? key : key.substr(colon + 1);
            return DataRoot() / ns / "loot_table" / (path + ".json");
        }

        // Caller holds g_mutex.
        Table* Lookup(const std::string& rawKey) {
            const std::string key = WithNamespace(rawKey);
            auto it = g_tables.find(key);
            if (it != g_tables.end()) return it->second.get();
            std::unique_ptr<Table> table;
            const std::filesystem::path file = PathFor(key);
            std::ifstream in(file);
            if (in) {
                nlohmann::json j;
                try {
                    in >> j;
                    table = ParseTable(key, j);
                } catch (const std::exception& e) {
                    Log::Warning("[ChestLoot] %s: %s", file.string().c_str(), e.what());
                }
            } else {
                Log::Warning("[ChestLoot] no loot table '%s' (%s)", key.c_str(), file.string().c_str());
            }
            Table* result = table.get();
            g_tables[key] = std::move(table);
            return result;
        }

        // ── Evaluation (MC LootTable / LootPool / LootPoolSingletonContainer) ──

        struct Context {
            JavaRandom& random;
            float luck = 0.0f;                 // LootContext.getLuck — the opener's LUCK
            std::set<std::string> visiting;    // loot_table refs: no cycles
            const LootLevelContext* level = nullptr;   // the level + ORIGIN, when the roll has them
        };

        bool Test(const std::vector<Condition>& conditions, Context& ctx, Table& table);

        bool TestOne(const Condition& c, Context& ctx, Table& table) {
            return Test(std::vector<Condition>{c}, ctx, table);
        }

        bool Test(const std::vector<Condition>& conditions, Context& ctx, Table& table) {
            for (const Condition& c : conditions) {
                switch (c.kind) {
                    case Condition::Kind::RandomChance:
                        if (!(ctx.random.NextFloat() < c.chance)) return false;
                        break;
                    case Condition::Kind::Inverted:
                        if (c.terms.empty() || TestOne(c.terms.front(), ctx, table)) return false;
                        break;
                    case Condition::Kind::AllOf:
                        if (!Test(c.terms, ctx, table)) return false;
                        break;
                    case Condition::Kind::AnyOf: {
                        bool any = false;
                        for (const Condition& term : c.terms) {
                            if (TestOne(term, ctx, table)) { any = true; break; }
                        }
                        if (!any) return false;
                        break;
                    }
                    case Condition::Kind::LocationCheck: {
                        // LocationCheck.test: ORIGIN + offset in the biome
                        // set; no ORIGIN (a /loot roll) never matches.
                        if (!ctx.level || !ctx.level->origin) return false;
                        const glm::dvec3& o = *ctx.level->origin;
                        const glm::ivec3 pos(static_cast<int>(std::floor(o.x)) + c.offset.x,
                                             static_cast<int>(std::floor(o.y)) + c.offset.y,
                                             static_cast<int>(std::floor(o.z)) + c.offset.z);
                        if (!MapItemBridge::BiomeIn(ctx.level->dimensionId, pos, c.biomes)) return false;
                        break;
                    }
                    case Condition::Kind::FishingHookOpenWater: {
                        // EntityPredicate.matches(null) is false: a roll with
                        // no hook as THIS never passes.
                        if (!ctx.level || !ctx.level->fishingHookInOpenWater) return false;
                        if (c.hasInOpenWater && *ctx.level->fishingHookInOpenWater != c.inOpenWater) return false;
                        break;
                    }
                    case Condition::Kind::Unsupported:
                        if (!table.warnedUnsupported) {
                            table.warnedUnsupported = true;
                            Log::Warning("[ChestLoot] %s: condition '%s' is not modelled; treating as true",
                                         table.key.c_str(), c.name.c_str());
                        }
                        break;
                }
            }
            return true;
        }

        // OMINOUS_BOTTLE_AMPLIFIER as a patch: a value equal to the item's
        // own default is no patch at all (PatchedDataComponentMap.set), so a
        // looted level-0 bottle stacks with a crafted one.
        void SetOminousBottleAmplifier(ItemStack& stack, int amplifier) {
            const auto prototype = ItemRegistry::Get(stack.itemId)
                                       .defaultComponents.get(DataComponents::OMINOUS_BOTTLE_AMPLIFIER);
            if (prototype && *prototype == amplifier) {
                stack.components.remove(DataComponents::OMINOUS_BOTTLE_AMPLIFIER);
            } else {
                stack.components.set(DataComponents::OMINOUS_BOTTLE_AMPLIFIER, amplifier);
            }
        }

        // One entry of a DataComponentPatch in its JSON form, for the item
        // components this engine carries. "!id" removes the component. False
        // for a component the engine has no field for (armor trims, …).
        bool ApplyComponent(ItemStack& stack, const std::string& rawId, const nlohmann::json& value) {
            const bool removal = !rawId.empty() && rawId[0] == '!';
            const std::string id = StripNamespace(removal ? rawId.substr(1) : rawId);
            const auto intOf = [&value](int& out) {
                if (value.is_number_integer()) { out = value.get<int>(); return true; }
                if (value.is_object() && value.contains("value") && value["value"].is_number_integer()) {
                    out = value["value"].get<int>();
                    return true;
                }
                return false;
            };
            int n = 0;
            if (id == "ominous_bottle_amplifier") {
                if (removal) { stack.components.remove(DataComponents::OMINOUS_BOTTLE_AMPLIFIER); return true; }
                if (!intOf(n)) return false;
                SetOminousBottleAmplifier(stack, std::clamp(n, 0, DataComponents::kOminousBottleMaxAmplifier));
                return true;
            }
            if (id == "custom_name" || id == "item_name") {
                const auto& type = id == "custom_name" ? DataComponents::CUSTOM_NAME : DataComponents::ITEM_NAME;
                if (removal) { stack.components.remove(type); return true; }
                const std::string text = ParseText(value);
                if (text.empty()) return false;
                stack.components.set(type, text);
                return true;
            }
            if (id == "rarity") {
                if (removal) { stack.components.remove(DataComponents::RARITY); return true; }
                if (!value.is_string()) return false;
                const std::string r = value.get<std::string>();
                Rarity rarity;
                if (r == "common") rarity = Rarity::COMMON;
                else if (r == "uncommon") rarity = Rarity::UNCOMMON;
                else if (r == "rare") rarity = Rarity::RARE;
                else if (r == "epic") rarity = Rarity::EPIC;
                else return false;
                stack.components.set(DataComponents::RARITY, rarity);
                return true;
            }
            if (id == "dyed_color") {
                if (removal) { stack.components.remove(DataComponents::DYED_COLOR); return true; }
                if (value.is_object() && value.contains("rgb") && value["rgb"].is_number_integer()) {
                    stack.components.set(DataComponents::DYED_COLOR, value["rgb"].get<int32_t>());
                    return true;
                }
                if (!intOf(n)) return false;
                stack.components.set(DataComponents::DYED_COLOR, static_cast<int32_t>(n));
                return true;
            }
            if (id == "damage" || id == "max_damage" || id == "repair_cost" || id == "enchantable") {
                const auto& type = id == "damage" ? DataComponents::DAMAGE
                                 : id == "max_damage" ? DataComponents::MAX_DAMAGE
                                 : id == "repair_cost" ? DataComponents::REPAIR_COST
                                                       : DataComponents::ENCHANTABLE;
                if (removal) { stack.components.remove(type); return true; }
                if (!intOf(n)) return false;
                stack.components.set(type, static_cast<int32_t>(n));
                return true;
            }
            if (id == "unbreakable") {
                if (removal) stack.components.remove(DataComponents::UNBREAKABLE);
                else         stack.components.set(DataComponents::UNBREAKABLE, true);
                return true;
            }
            if (id == "enchantment_glint_override") {
                if (removal) { stack.components.remove(DataComponents::ENCHANTMENT_GLINT_OVERRIDE); return true; }
                if (!value.is_boolean()) return false;
                stack.components.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, value.get<bool>());
                return true;
            }
            return false;
        }

        void Apply(const Function& fn, ItemStack& stack, Context& ctx, Table& table) {
            if (!Test(fn.conditions, ctx, table)) return;
            switch (fn.kind) {
                case Function::Kind::SetCount: {
                    // MC SetItemCountFunction.run: setCount(add ? count + n : n).
                    const int n = fn.count.GetInt(ctx.random);
                    stack.count = fn.add ? stack.count + n : n;
                    if (stack.count <= 0) stack.Clear();
                    break;
                }
                case Function::Kind::SetDamage: {
                    // MC SetItemDamageFunction.run: `damage` is the fraction
                    // of durability REMAINING (added to what is left when
                    // `add`), clamped to 0..1; a non-damageable item is left
                    // alone (MC only logs).
                    if (!IsDamageableItem(stack)) break;
                    const int maxDamage = GetMaxDamage(stack);
                    const float oldDamage = fn.add
                        ? 1.0f - static_cast<float>(GetDamageValue(stack)) / static_cast<float>(maxDamage)
                        : 0.0f;
                    const float damage = 1.0f - std::clamp(fn.count.GetFloat(ctx.random) + oldDamage, 0.0f, 1.0f);
                    SetDamageValue(stack, static_cast<int>(std::floor(damage * static_cast<float>(maxDamage))));
                    break;
                }
                case Function::Kind::SetName:
                    if (fn.text.empty()) break;
                    stack.components.set(fn.itemName ? DataComponents::ITEM_NAME : DataComponents::CUSTOM_NAME,
                                         fn.text);
                    break;
                case Function::Kind::ExplorationMap:
                    // ExplorationMapFunction.run: needs the level and ORIGIN;
                    // without them the item passes through unchanged.
                    if (ctx.level && ctx.level->origin) {
                        stack = MapItemBridge::ApplyExplorationMap(stack, ctx.level->dimensionId,
                                                                   *ctx.level->origin, fn.exploration);
                    }
                    break;
                case Function::Kind::Filtered: {
                    // FilteredFunction.run: the predicate, then on_pass /
                    // on_fail (an absent branch leaves the stack alone).
                    bool pass = fn.filterItems.empty() ||
                                std::find(fn.filterItems.begin(), fn.filterItems.end(), stack.itemId) !=
                                    fn.filterItems.end();
                    if (pass && fn.filterNeedsMapId) pass = stack.get(DataComponents::MAP_ID).has_value();
                    for (const Function& next : pass ? fn.onPass : fn.onFail) {
                        if (stack.IsEmpty()) break;
                        Apply(next, stack, ctx, table);
                    }
                    break;
                }
                case Function::Kind::Discard:
                    // DiscardItem: the stack is gone.
                    stack.Clear();
                    break;
                case Function::Kind::EnchantRandomly: {
                    // MC EnchantRandomlyFunction.run.
                    const bool targetIsBook = stack.itemId == Items::Book;
                    const bool checkCompat  = !targetIsBook && fn.onlyCompatible;
                    const std::vector<EnchantmentId>& source =
                        fn.hasOptions ? fn.options : EnchantmentDefinitions::All();
                    std::vector<EnchantmentId> compatible;
                    for (const EnchantmentId id : source) {
                        if (!checkCompat || EnchantmentDefinitions::IsSupportedItem(id, stack.itemId)) {
                            compatible.push_back(id);
                        }
                    }
                    if (compatible.empty()) break;   // Util.getRandomSafe → empty
                    const EnchantmentId id = compatible[static_cast<size_t>(
                        ctx.random.NextInt(static_cast<int>(compatible.size())))];
                    const auto& d = EnchantmentDefinitions::Get(id);
                    const int level = ctx.random.NextInt(1, std::max(1, d.maxLevel));
                    if (targetIsBook) stack = ItemStack(Items::EnchantedBook, 1);   // new ItemStack(ENCHANTED_BOOK)
                    EnchantmentHelper::Enchant(stack, id, level);
                    break;
                }
                case Function::Kind::EnchantWithLevels: {
                    // MC EnchantWithLevelsFunction.run → EnchantmentHelper.enchantItem.
                    const int cost = fn.count.GetInt(ctx.random);
                    stack = EnchantmentHelper::EnchantItem(
                        ctx.random, stack, cost, fn.hasOptions ? fn.options : EnchantmentDefinitions::All());
                    break;
                }
                case Function::Kind::SetEnchantments: {
                    // MC SetEnchantmentsFunction.run (book → enchanted book).
                    if (stack.itemId == Items::Book) stack = ItemStack(Items::EnchantedBook, stack.count);
                    for (const auto& [id, provider] : fn.enchantments) {
                        EnchantmentHelper::Enchant(stack, id, provider.GetInt(ctx.random));
                    }
                    break;
                }
                case Function::Kind::SetPotion:
                    // MC SetPotionFunction.run: update(POTION_CONTENTS, EMPTY,
                    // potion, withPotion) — keeps any custom effects/colour.
                    stack.components.set(DataComponents::POTION_CONTENTS,
                                         GetPotionContents(stack).WithPotion(fn.potion));
                    break;
                case Function::Kind::SetStewEffect: {
                    // MC SetStewEffectFunction.run: suspicious stew only; ONE
                    // random entry, its duration in seconds (x20 unless the
                    // effect is instantaneous), appended to the stew's list.
                    if (stack.itemId != Items::SuspiciousStew || fn.stewEffects.empty()) break;
                    const auto& entry = fn.stewEffects[static_cast<size_t>(
                        ctx.random.NextInt(static_cast<int>(fn.stewEffects.size())))];
                    int duration = entry.second.GetInt(ctx.random);
                    if (!IsInstantenousEffect(entry.first)) duration *= 20;
                    SuspiciousStewEffects effects =
                        stack.get(DataComponents::SUSPICIOUS_STEW_EFFECTS).value_or(SuspiciousStewEffects{});
                    effects.effects.push_back({entry.first, duration});
                    stack.components.set(DataComponents::SUSPICIOUS_STEW_EFFECTS, effects);
                    break;
                }
                case Function::Kind::SetWrittenBookPages: {
                    // MC SetWrittenBookPagesFunction.run: update(
                    // WRITTEN_BOOK_CONTENT, EMPTY, …withReplacedPages(
                    // mode.apply(pages, newPages))) — on whatever item it is
                    // given; the replaced pages are unresolved again.
                    const WrittenBookContent original =
                        stack.get(DataComponents::WRITTEN_BOOK_CONTENT).value_or(WrittenBookContent::Empty());
                    stack.components.set(DataComponents::WRITTEN_BOOK_CONTENT,
                        original.WithReplacedPages(fn.pageOperation.Apply(
                            original.pages, fn.writtenPages, static_cast<size_t>(INT_MAX), table.key)));
                    break;
                }
                case Function::Kind::SetWritableBookPages: {
                    // MC SetWritableBookPagesFunction.run (at most 100 pages).
                    WritableBookContent content =
                        stack.get(DataComponents::WRITABLE_BOOK_CONTENT).value_or(WritableBookContent{});
                    content.pages = fn.pageOperation.Apply(content.pages, fn.writablePages,
                                                           WritableBookContent::MAX_PAGES, table.key);
                    stack.components.set(DataComponents::WRITABLE_BOOK_CONTENT, std::move(content));
                    break;
                }
                case Function::Kind::SetBookCover: {
                    // MC SetBookCoverFunction.run: the present fields replace
                    // the book's; pages and resolved are kept.
                    WrittenBookContent content =
                        stack.get(DataComponents::WRITTEN_BOOK_CONTENT).value_or(WrittenBookContent::Empty());
                    if (fn.coverTitle)      content.title      = *fn.coverTitle;
                    if (fn.coverAuthor)     content.author     = *fn.coverAuthor;
                    if (fn.coverGeneration) content.generation = *fn.coverGeneration;
                    stack.components.set(DataComponents::WRITTEN_BOOK_CONTENT, std::move(content));
                    break;
                }
                case Function::Kind::SetOminousBottleAmplifier:
                    // MC SetOminousBottleAmplifierFunction.run: the provider,
                    // clamped to 0..4, set on whatever item it is given.
                    SetOminousBottleAmplifier(stack, std::clamp(fn.count.GetInt(ctx.random), 0,
                                                                DataComponents::kOminousBottleMaxAmplifier));
                    break;
                case Function::Kind::SetInstrument:
                    // MC SetInstrumentFunction.run: options.getRandomElement
                    // (random) - list.get(random.nextInt(size)), nothing for
                    // an empty set - then itemStack.set(INSTRUMENT, ...). A
                    // value equal to the item's default leaves no patch
                    // (PatchedDataComponentMap drops it).
                    if (!fn.instrumentOptions.empty()) {
                        const int index = ctx.random.NextInt(static_cast<int>(fn.instrumentOptions.size()));
                        const std::string& chosen = fn.instrumentOptions[static_cast<size_t>(index)];
                        const auto prototype =
                            ItemRegistry::Get(stack.itemId).defaultComponents.get(DataComponents::INSTRUMENT);
                        if (prototype && *prototype == chosen) {
                            stack.components.remove(DataComponents::INSTRUMENT);
                        } else {
                            stack.components.set(DataComponents::INSTRUMENT, chosen);
                        }
                    }
                    break;
                case Function::Kind::SetComponents:
                    // MC SetComponentsFunction.run → applyComponentsAndValidate.
                    for (const auto& [id, value] : fn.components) {
                        if (!ApplyComponent(stack, id, value) && !table.warnedUnsupported) {
                            table.warnedUnsupported = true;
                            Log::Debug("[ChestLoot] %s: set_components '%s' has no item component to write",
                                       table.key.c_str(), id.c_str());
                        }
                    }
                    break;
                case Function::Kind::Unsupported:
                    if (!table.warnedUnsupported) {
                        table.warnedUnsupported = true;
                        Log::Debug("[ChestLoot] %s: function '%s' has no item component to write; the bare item drops",
                                   table.key.c_str(), fn.name.c_str());
                    }
                    break;
            }
        }

        void ApplyAll(const std::vector<Function>& fns, ItemStack& stack, Context& ctx, Table& table) {
            for (const Function& fn : fns) {
                if (stack.IsEmpty()) return;
                Apply(fn, stack, ctx, table);
            }
        }

        void GetRandomItemsRaw(Table& table, Context& ctx, std::vector<ItemStack>& out);

        // MC LootPoolSingletonContainer.createItemStack + LootTableReference.
        // Functions decorate outward: entry, then pool, then table.
        void CreateItemStacks(const Entry& entry, const Pool& pool, Table& table, Context& ctx,
                              std::vector<ItemStack>& out) {
            std::vector<ItemStack> produced;
            switch (entry.kind) {
                case Entry::Kind::Item:
                    produced.emplace_back(entry.item, 1);
                    break;
                case Entry::Kind::LootTableRef: {
                    const auto rollRef = [&](const std::string& key) {
                        if (key.empty() || ctx.visiting.count(key)) return;   // MC: "Detected infinite loop"
                        Table* ref = Lookup(key);
                        if (!ref) return;
                        ctx.visiting.insert(key);
                        GetRandomItemsRaw(*ref, ctx, produced);
                        ctx.visiting.erase(key);
                    };
                    rollRef(entry.tableRef);
                    for (const std::string& key : entry.moreTableRefs) rollRef(key);
                    for (const std::shared_ptr<Table>& inlineTable : entry.inlineTables) {
                        if (inlineTable) GetRandomItemsRaw(*inlineTable, ctx, produced);
                    }
                    break;
                }
                case Entry::Kind::Empty:
                case Entry::Kind::Unsupported:
                    break;
            }
            for (ItemStack& stack : produced) {
                ApplyAll(entry.functions, stack, ctx, table);
                ApplyAll(pool.functions,  stack, ctx, table);
                ApplyAll(table.functions, stack, ctx, table);
                if (!stack.IsEmpty()) out.push_back(std::move(stack));
            }
        }

        // MC LootPoolEntryContainer.getWeight: max(floor(weight + quality * luck), 0).
        int WeightOf(const Entry& e, float luck) {
            return std::max(static_cast<int>(std::floor(static_cast<float>(e.weight)
                                                        + static_cast<float>(e.quality) * luck)), 0);
        }

        // MC LootPool.addRandomItem.
        void AddRandomItem(const Pool& pool, Table& table, Context& ctx, std::vector<ItemStack>& out) {
            std::vector<const Entry*> valid;
            int totalWeight = 0;
            for (const Entry& e : pool.entries) {
                if (!Test(e.conditions, ctx, table)) continue;   // LootPoolEntryContainer.expand
                const int weight = WeightOf(e, ctx.luck);
                if (weight > 0) { valid.push_back(&e); totalWeight += weight; }
            }
            if (totalWeight == 0 || valid.empty()) return;
            if (valid.size() == 1) { CreateItemStacks(*valid[0], pool, table, ctx, out); return; }
            int index = ctx.random.NextInt(totalWeight);
            for (const Entry* e : valid) {
                index -= WeightOf(*e, ctx.luck);
                if (index < 0) { CreateItemStacks(*e, pool, table, ctx, out); return; }
            }
        }

        // MC LootPool.addRandomItems.
        void AddRandomItems(const Pool& pool, Table& table, Context& ctx, std::vector<ItemStack>& out) {
            if (!Test(pool.conditions, ctx, table)) return;
            const int count = pool.rolls.GetInt(ctx.random)
                            + static_cast<int>(std::floor(pool.bonusRolls.GetFloat(ctx.random) * ctx.luck));
            for (int i = 0; i < count; ++i) AddRandomItem(pool, table, ctx, out);
        }

        // MC LootTable.getRandomItemsRaw (table functions are applied per
        // stack in CreateItemStacks, where MC's decorate chain puts them).
        void GetRandomItemsRaw(Table& table, Context& ctx, std::vector<ItemStack>& out) {
            for (const Pool& pool : table.pools) AddRandomItems(pool, table, ctx, out);
        }

        // MC LootTable.createStackSplitter: a stack past its max size becomes
        // several.
        void SplitStacks(std::vector<ItemStack>& stacks) {
            std::vector<ItemStack> out;
            for (ItemStack& stack : stacks) {
                if (stack.IsEmpty()) continue;
                const int max = std::max(1, Game::GetMaxStackSize(stack));
                if (stack.count < max) { out.push_back(std::move(stack)); continue; }
                int count = stack.count;
                while (count > 0) {
                    ItemStack copy = stack;
                    copy.count = std::min(max, count);
                    count -= copy.count;
                    out.push_back(std::move(copy));
                }
            }
            stacks = std::move(out);
        }

        // MC Util.shuffle: Fisher–Yates from the end with nextInt(i).
        template <class T>
        void Shuffle(std::vector<T>& list, JavaRandom& random) {
            for (int i = static_cast<int>(list.size()); i > 1; --i) {
                const int swapTo = random.NextInt(i);
                std::swap(list[static_cast<size_t>(i - 1)], list[static_cast<size_t>(swapTo)]);
            }
        }

        // MC LootTable.shuffleAndSplitItems.
        void ShuffleAndSplitItems(std::vector<ItemStack>& result, int availableSlots, JavaRandom& random) {
            std::vector<ItemStack> splittable;
            for (auto it = result.begin(); it != result.end();) {
                if (it->IsEmpty()) {
                    it = result.erase(it);
                } else if (it->count > 1) {
                    splittable.push_back(std::move(*it));
                    it = result.erase(it);
                } else {
                    ++it;
                }
            }
            while (availableSlots - static_cast<int>(result.size()) - static_cast<int>(splittable.size()) > 0
                   && !splittable.empty()) {
                const int pick = random.NextInt(0, static_cast<int>(splittable.size()) - 1);
                ItemStack stack = std::move(splittable[static_cast<size_t>(pick)]);
                splittable.erase(splittable.begin() + pick);
                const int remove = random.NextInt(1, stack.count / 2);
                ItemStack copy = stack;            // ItemStack.split(remove)
                copy.count = remove;
                stack.count -= remove;
                if (stack.count > 1 && random.NextBool()) splittable.push_back(std::move(stack));
                else result.push_back(std::move(stack));
                if (copy.count > 1 && random.NextBool()) splittable.push_back(std::move(copy));
                else result.push_back(std::move(copy));
            }
            result.insert(result.end(), std::make_move_iterator(splittable.begin()),
                          std::make_move_iterator(splittable.end()));
            Shuffle(result, random);
        }

        // MC LootTable.getAvailableSlots.
        std::vector<int> AvailableSlots(const IContainer& container, JavaRandom& random) {
            std::vector<int> slots;
            for (int i = 0; i < container.GetContainerSize(); ++i) {
                if (container.GetItem(i).IsEmpty()) slots.push_back(i);
            }
            Shuffle(slots, random);
            return slots;
        }

    } // namespace

    bool Fill(IContainer& container, const std::string& key, int64_t seed, JavaRandom* levelRandom,
              float luck, const LootLevelContext* level) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Table* table = Lookup(key);
        if (!table) return false;

        // LootContext.Builder.withOptionalRandomSeed: a non-zero seed gets its
        // own java.util.Random; otherwise the level's. (MC would then prefer
        // the table's random_sequence; there is no sequence store here.)
        JavaRandom seeded(seed);
        JavaRandom fallback(static_cast<int64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        JavaRandom& random = seed != 0 ? seeded : (levelRandom ? *levelRandom : fallback);
        Context ctx{random};
        ctx.level = level;
        // LootParams.Builder.withLuck(player.getLuck()) — RandomizableContainer
        // .unpackLootTable passes the opening player's LUCK attribute (LUCK /
        // UNLUCK effects, +-1 a level); 0 with no player. It weights every
        // entry by `quality` and adds bonus_rolls * luck rolls.
        ctx.luck = luck;

        // LootTable.fill.
        std::vector<ItemStack> stacks;
        GetRandomItemsRaw(*table, ctx, stacks);
        SplitStacks(stacks);
        std::vector<int> slots = AvailableSlots(container, random);
        ShuffleAndSplitItems(stacks, static_cast<int>(slots.size()), random);
        for (ItemStack& stack : stacks) {
            if (slots.empty()) {
                Log::Warning("[ChestLoot] %s: tried to over-fill a container", table->key.c_str());
                break;
            }
            const int slot = slots.back();
            slots.pop_back();
            container.SetItem(slot, stack.IsEmpty() ? ItemStack{} : stack);
        }
        return true;
    }

    bool GetRandomItems(const std::string& key, JavaRandom& random, float luck, std::vector<ItemStack>& out,
                        const LootLevelContext* level) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Table* table = Lookup(key);
        if (!table) return false;
        Context ctx{random};
        ctx.luck = luck;
        ctx.level = level;
        // LootTable.getRandomItems: getRandomItemsRaw through
        // createStackSplitter — no slot shuffling, that is fill()'s.
        std::vector<ItemStack> stacks;
        GetRandomItemsRaw(*table, ctx, stacks);
        SplitStacks(stacks);
        for (ItemStack& stack : stacks) out.push_back(std::move(stack));
        return true;
    }

    bool ApplyItemModifier(const nlohmann::json& modifier, ItemStack& stack, JavaRandom& random,
                           std::string& error, const LootLevelContext* level) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Table table;
        table.key = "item_modifier";
        table.functions = ParseFunctions(modifier);
        if (table.functions.empty()) {
            error = "Not a valid item modifier";
            return false;
        }
        for (const Function& fn : table.functions) {
            if (fn.kind == Function::Kind::Unsupported) {
                error = "Unknown item modifier function 'minecraft:" + fn.name + "'";
                return false;
            }
        }
        Context ctx{random};
        ctx.level = level;
        ApplyAll(table.functions, stack, ctx, table);
        return true;
    }

    bool Exists(const std::string& key) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return Lookup(key) != nullptr;
    }

    void Reload() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tables.clear();
    }

} // namespace Game::ChestLoot
