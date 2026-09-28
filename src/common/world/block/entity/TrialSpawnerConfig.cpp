// File: src/common/world/block/entity/TrialSpawnerConfig.cpp
//
// MC TrialSpawnerConfig.DIRECT_CODEC / SpawnData.CODEC / WeightedList.codec
// over the 26.3 datapack JSON, and the TRIAL_SPAWNER_CONFIG registry cache.
// See the header.
#include "TrialSpawnerConfig.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/EntityType.hpp"
#include "common/nbt/NbtWrite.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace Game {

    namespace {

        using Json = nlohmann::json;

        // Same rule as the loot tables and DataTags: MC_DATA_ROOT, else ./data.
        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        std::string_view StripNamespace(std::string_view id) {
            const size_t colon = id.find(':');
            return colon == std::string_view::npos ? id : id.substr(colon + 1);
        }

        // ── JSON → NBT (the entity compound) ────────────────────────────────
        //
        // MC decodes SpawnData.entity with CompoundTag.CODEC, i.e. the JSON is
        // converted to NBT. Integers become Int tags and fractional numbers
        // Double tags; every reader of an entity compound (the engine's and
        // vanilla's) reads numbers through the numeric-tolerant getters, so a
        // "Size": 1 reaches the slime whichever width it is stored at.

        Nbt::TagType ListElementType(const Json& array) {
            if (array.empty()) return Nbt::TagType::End;
            bool anyFloat = false;
            for (const Json& e : array) if (e.is_number_float()) anyFloat = true;
            const Json& first = array.front();
            if (first.is_boolean())        return Nbt::TagType::Byte;
            if (first.is_number())         return anyFloat ? Nbt::TagType::Double : Nbt::TagType::Int;
            if (first.is_string())         return Nbt::TagType::String;
            if (first.is_array())          return Nbt::TagType::List;
            return Nbt::TagType::Compound;
        }

        void WriteJsonValue(Nbt::Writer& w, std::string_view name, const Json& v);

        void WriteJsonListElement(Nbt::Writer& w, Nbt::Writer::ListScope& list, Nbt::TagType type,
                                  const Json& e) {
            switch (type) {
                case Nbt::TagType::Byte:   w.ListByte(list, e.is_boolean() ? (e.get<bool>() ? 1 : 0) : 0); break;
                case Nbt::TagType::Int:    w.ListInt(list, e.is_number() ? e.get<int32_t>() : 0); break;
                case Nbt::TagType::Double: w.ListDouble(list, e.is_number() ? e.get<double>() : 0.0); break;
                case Nbt::TagType::String: w.ListString(list, e.is_string() ? e.get<std::string>() : std::string()); break;
                case Nbt::TagType::List: {
                    const Json empty = Json::array();
                    const Json& inner = e.is_array() ? e : empty;
                    const Nbt::TagType innerType = ListElementType(inner);
                    auto nested = w.ListListBegin(list, innerType);
                    for (const Json& x : inner) WriteJsonListElement(w, nested, innerType, x);
                    w.EndList(nested);
                    break;
                }
                case Nbt::TagType::Compound:
                    w.ListCompoundBegin(list);
                    if (e.is_object()) {
                        for (auto it = e.begin(); it != e.end(); ++it) WriteJsonValue(w, it.key(), it.value());
                    }
                    w.ListCompoundEnd(list);
                    break;
                default:
                    break;
            }
        }

        void WriteJsonValue(Nbt::Writer& w, std::string_view name, const Json& v) {
            if (v.is_boolean()) {
                w.Bool(name, v.get<bool>());
            } else if (v.is_number_integer() || v.is_number_unsigned()) {
                const int64_t n = v.get<int64_t>();
                if (n >= std::numeric_limits<int32_t>::min() && n <= std::numeric_limits<int32_t>::max()) {
                    w.Int(name, static_cast<int32_t>(n));
                } else {
                    w.Long(name, n);
                }
            } else if (v.is_number_float()) {
                w.Double(name, v.get<double>());
            } else if (v.is_string()) {
                w.String(name, v.get<std::string>());
            } else if (v.is_array()) {
                const Nbt::TagType type = ListElementType(v);
                auto list = w.BeginList(name, type);
                for (const Json& e : v) WriteJsonListElement(w, list, type, e);
                w.EndList(list);
            } else if (v.is_object()) {
                w.BeginCompound(name);
                for (auto it = v.begin(); it != v.end(); ++it) WriteJsonValue(w, it.key(), it.value());
                w.EndCompound();
            }
            // null: no NBT form; MC's conversion drops it as well.
        }

        // ── SpawnData.CODEC ─────────────────────────────────────────────────

        // MC InclusiveRange.INT's codec (ExtraCodecs.intervalCodec): a single
        // int, a two-element [min, max] list, or {min_inclusive,
        // max_inclusive}; min > max is refused.
        bool ReadIntRange(const Json& j, int& lo, int& hi) {
            int a = 0, b = 0;
            if (j.is_number_integer()) {
                a = b = j.get<int>();
            } else if (j.is_array() && j.size() == 2 && j[0].is_number_integer() && j[1].is_number_integer()) {
                a = j[0].get<int>();
                b = j[1].get<int>();
            } else if (j.is_object() && j.contains("min_inclusive") && j.contains("max_inclusive") &&
                       j["min_inclusive"].is_number_integer() && j["max_inclusive"].is_number_integer()) {
                a = j["min_inclusive"].get<int>();
                b = j["max_inclusive"].get<int>();
            } else {
                return false;
            }
            if (a > b) return false;
            lo = a;
            hi = b;
            return true;
        }

        // MC CustomSpawnRules.lightLimit: lenientOptionalFieldOf(name, [0,15])
        // then validated against 0..15.
        bool ReadLightLimit(const Json& rules, const char* key, int& lo, int& hi) {
            lo = 0;
            hi = 15;
            if (rules.contains(key) && !ReadIntRange(rules[key], lo, hi)) {
                lo = 0;
                hi = 15;
            }
            return lo >= 0 && hi <= 15;
        }

        // MC EquipmentSlot.CODEC names, by ordinal (EquipmentSlot.hpp).
        int EquipmentSlotFromName(std::string_view name) {
            static constexpr std::string_view kNames[] = {
                "mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"
            };
            for (int i = 0; i < 8; ++i) if (kNames[i] == name) return i;
            return -1;
        }

        // MC EquipmentTable.CODEC: {loot_table, slot_drop_chances?}. The
        // chances are Codec.either(one FLOAT for every slot, a slot -> FLOAT
        // map); absent is the empty map — no slot's chance is touched.
        bool ReadEquipment(const Json& j, SpawnData& out) {
            if (!j.is_object() || !j.contains("loot_table") || !j["loot_table"].is_string()) return false;
            out.hasEquipment = true;
            out.equipmentLootTable = WithNamespace(j["loot_table"].get<std::string>());
            out.equipmentUniformDrop = false;
            for (float& slot : out.equipmentDropChances) slot = std::numeric_limits<float>::quiet_NaN();
            if (!j.contains("slot_drop_chances")) return true;
            const Json& chances = j["slot_drop_chances"];
            if (chances.is_number()) {
                out.equipmentUniformDrop = true;
                const float c = chances.get<float>();
                for (float& slot : out.equipmentDropChances) slot = c;
                return true;
            }
            if (!chances.is_object()) return false;
            for (auto it = chances.begin(); it != chances.end(); ++it) {
                const int slot = EquipmentSlotFromName(StripNamespace(it.key()));
                if (slot < 0 || !it.value().is_number()) return false;
                out.equipmentDropChances[slot] = it.value().get<float>();
            }
            return true;
        }

        bool ReadSpawnData(const Json& j, SpawnData& out, std::string& error) {
            out = SpawnData{};
            if (!j.is_object() || !j.contains("entity") || !j["entity"].is_object()) {
                error = "SpawnData without an entity compound";
                return false;
            }
            const Json& entity = j["entity"];
            {
                Nbt::Writer w;
                w.BeginRootCompound();
                for (auto it = entity.begin(); it != entity.end(); ++it) WriteJsonValue(w, it.key(), it.value());
                w.EndRootCompound();
                if (!w.ok()) {
                    error = "SpawnData entity compound does not encode";
                    return false;
                }
                out.entityNbt = w.TakeBytes();
            }
            if (entity.contains("id") && entity["id"].is_string()) {
                EntityTypeId type{};
                if (TrialSpawnerConfigs::EntityTypeFromId(entity["id"].get<std::string>(), type)) {
                    out.hasType = true;
                    out.type = type;
                }
            }
            out.bareId = entity.size() == 1 && entity.contains("id");
            if (entity.contains("Pos") && entity["Pos"].is_array() && entity["Pos"].size() == 3) {
                const Json& p = entity["Pos"];
                if (p[0].is_number() && p[1].is_number() && p[2].is_number()) {
                    out.hasPos = true;
                    out.pos = glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
                }
            }
            const auto numberOr = [&entity](const char* key, double fallback) {
                if (!entity.contains(key)) return fallback;
                const Json& v = entity[key];
                if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
                return v.is_number() ? v.get<double>() : fallback;
            };
            out.baby = numberOr("IsBaby", 0.0) != 0.0 || numberOr("Age", 0.0) < 0.0;

            if (j.contains("custom_spawn_rules")) {
                const Json& rules = j["custom_spawn_rules"];
                if (!rules.is_object()) {
                    error = "custom_spawn_rules is not a compound";
                    return false;
                }
                out.hasCustomRules = true;
                if (!ReadLightLimit(rules, "block_light_limit", out.blockLightMin, out.blockLightMax) ||
                    !ReadLightLimit(rules, "sky_light_limit", out.skyLightMin, out.skyLightMax)) {
                    error = "custom_spawn_rules light limit outside 0..15";
                    return false;
                }
            }
            if (j.contains("equipment") && !ReadEquipment(j["equipment"], out)) {
                error = "equipment does not decode";
                return false;
            }
            return true;
        }

        // MC Weighted.codec: {data, weight} with weight a non-negative int.
        template <class ReadData>
        bool ReadWeightedList(const Json& j, ReadData&& readData, std::string& error) {
            if (!j.is_array()) {
                error = "weighted list is not a list";
                return false;
            }
            for (const Json& entry : j) {
                if (!entry.is_object() || !entry.contains("data") || !entry.contains("weight") ||
                    !entry["weight"].is_number_integer() || entry["weight"].get<int64_t>() < 0 ||
                    entry["weight"].get<int64_t>() > std::numeric_limits<int32_t>::max()) {
                    error = "weighted entry needs data and a non-negative int weight";
                    return false;
                }
                if (!readData(entry["data"], entry["weight"].get<int>())) return false;
            }
            return true;
        }

        bool ReadFloatField(const Json& j, const char* key, float& out, std::string& error) {
            if (!j.contains(key)) return true;
            if (!j[key].is_number()) {
                error = std::string(key) + " is not a number";
                return false;
            }
            const float v = j[key].get<float>();
            if (!(v >= 0.0f)) {   // floatRange(0, MAX_VALUE) — NaN fails too
                error = std::string(key) + " must be non-negative";
                return false;
            }
            out = v;
            return true;
        }

        bool ReadIntField(const Json& j, const char* key, int lo, int hi, int& out, std::string& error) {
            if (!j.contains(key)) return true;
            if (!j[key].is_number_integer()) {
                error = std::string(key) + " is not an int";
                return false;
            }
            const int64_t v = j[key].get<int64_t>();
            if (v < lo || v > hi) {
                error = std::string(key) + " out of range";
                return false;
            }
            out = static_cast<int>(v);
            return true;
        }

        bool ParseConfig(const Json& j, TrialSpawnerConfig& out, std::string& error) {
            out = TrialSpawnerConfig{};
            if (!j.is_object()) {
                error = "config is not an object";
                return false;
            }
            if (!ReadIntField(j, "spawn_range", 1, 128, out.spawnRange, error)) return false;
            if (!ReadFloatField(j, "total_mobs", out.totalMobs, error)) return false;
            if (!ReadFloatField(j, "simultaneous_mobs", out.simultaneousMobs, error)) return false;
            if (!ReadFloatField(j, "total_mobs_added_per_player", out.totalMobsAddedPerPlayer, error)) return false;
            if (!ReadFloatField(j, "simultaneous_mobs_added_per_player",
                                out.simultaneousMobsAddedPerPlayer, error)) return false;
            if (!ReadIntField(j, "ticks_between_spawn", 0, std::numeric_limits<int32_t>::max(),
                              out.ticksBetweenSpawn, error)) return false;
            if (j.contains("spawn_potentials")) {
                std::vector<WeightedSpawnData> potentials;
                const bool ok = ReadWeightedList(j["spawn_potentials"], [&](const Json& data, int weight) {
                    WeightedSpawnData w;
                    w.weight = weight;
                    if (!ReadSpawnData(data, w.data, error)) return false;
                    potentials.push_back(std::move(w));
                    return true;
                }, error);
                if (!ok) return false;
                out.spawnPotentials = std::move(potentials);
            }
            if (j.contains("loot_tables_to_eject")) {
                std::vector<WeightedLootTable> tables;
                const bool ok = ReadWeightedList(j["loot_tables_to_eject"], [&](const Json& data, int weight) {
                    if (!data.is_string()) {
                        error = "loot_tables_to_eject entry is not a loot table key";
                        return false;
                    }
                    tables.push_back({ WithNamespace(data.get<std::string>()), weight });
                    return true;
                }, error);
                if (!ok) return false;
                out.lootTablesToEject = std::move(tables);
            }
            if (j.contains("items_to_drop_when_ominous")) {
                if (!j["items_to_drop_when_ominous"].is_string()) {
                    error = "items_to_drop_when_ominous is not a loot table key";
                    return false;
                }
                out.itemsToDropWhenOminous = WithNamespace(j["items_to_drop_when_ominous"].get<std::string>());
            }
            return true;
        }

        // ── Registry cache ──────────────────────────────────────────────────

        std::mutex g_mutex;
        std::unordered_map<std::string, std::shared_ptr<const TrialSpawnerConfig>> g_configs;

        std::shared_ptr<const TrialSpawnerConfig> DefaultConfig() {
            static const std::shared_ptr<const TrialSpawnerConfig> config =
                std::make_shared<const TrialSpawnerConfig>();
            return config;
        }

        // MC WeightedList.getRandom: one nextInt(totalWeight) walked through
        // the cumulative weights; empty when the list weighs nothing.
        template <class T>
        int PickWeighted(const std::vector<T>& list, JavaRandom& random) {
            int64_t total = 0;
            for (const T& e : list) total += e.weight;
            if (total <= 0 || total > std::numeric_limits<int32_t>::max()) return -1;
            int roll = random.NextInt(static_cast<int32_t>(total));
            for (size_t i = 0; i < list.size(); ++i) {
                roll -= list[i].weight;
                if (roll < 0) return static_cast<int>(i);
            }
            return -1;
        }

    } // namespace

    // ── TrialSpawnerConfig ──────────────────────────────────────────────────

    int TrialSpawnerConfig::CalculateTargetTotalMobs(int additionalPlayers) const {
        return static_cast<int>(std::floor(static_cast<double>(
            totalMobs + totalMobsAddedPerPlayer * static_cast<float>(additionalPlayers))));
    }

    int TrialSpawnerConfig::CalculateTargetSimultaneousMobs(int additionalPlayers) const {
        return static_cast<int>(std::floor(static_cast<double>(
            simultaneousMobs + simultaneousMobsAddedPerPlayer * static_cast<float>(additionalPlayers))));
    }

    int TrialSpawnerConfig::PickSpawnPotential(JavaRandom& random) const {
        return PickWeighted(spawnPotentials, random);
    }

    std::string TrialSpawnerConfig::PickLootTableToEject(JavaRandom& random) const {
        const int i = PickWeighted(lootTablesToEject, random);
        return i < 0 ? std::string() : lootTablesToEject[static_cast<size_t>(i)].table;
    }

    // ── Holders ─────────────────────────────────────────────────────────────

    const TrialSpawnerConfig& TrialSpawnerConfigHolder::Get() const {
        return value ? *value : *DefaultConfig();
    }

    TrialSpawnerConfigHolder TrialSpawnerConfigHolder::Default() {
        TrialSpawnerConfigHolder h;
        h.value = DefaultConfig();
        return h;
    }

    TrialSpawnerConfigHolder TrialSpawnerConfigHolder::Reference(const std::string& key) {
        TrialSpawnerConfigHolder h;
        h.key = WithNamespace(key);
        h.value = TrialSpawnerConfigs::Lookup(h.key);
        if (!h.value) h.value = DefaultConfig();
        return h;
    }

    TrialSpawnerConfigHolder TrialSpawnerConfigHolder::Direct(TrialSpawnerConfig config) {
        TrialSpawnerConfigHolder h;
        h.value = std::make_shared<const TrialSpawnerConfig>(std::move(config));
        return h;
    }

    TrialSpawnerFullConfig TrialSpawnerFullConfig::OverrideEntity(EntityTypeId type) const {
        // MC TrialSpawnerConfig.withSpawning: every field kept, the
        // potentials replaced by the one bare SpawnData.
        auto withSpawning = [type](const TrialSpawnerConfig& base) {
            TrialSpawnerConfig c = base;
            c.spawnPotentials.clear();
            c.spawnPotentials.push_back({ TrialSpawnerConfigs::BareSpawnData(type), 1 });
            return TrialSpawnerConfigHolder::Direct(std::move(c));
        };
        TrialSpawnerFullConfig out;
        out.normal = withSpawning(normal.Get());
        out.ominous = withSpawning(ominous.Get());
        out.targetCooldownLength = targetCooldownLength;
        out.requiredPlayerRange = requiredPlayerRange;
        return out;
    }

    // ── Registry ────────────────────────────────────────────────────────────

    namespace TrialSpawnerConfigs {

        std::shared_ptr<const TrialSpawnerConfig> Lookup(const std::string& rawKey) {
            const std::string key = WithNamespace(rawKey);
            std::lock_guard<std::mutex> lock(g_mutex);
            if (auto it = g_configs.find(key); it != g_configs.end()) return it->second;

            const size_t colon = key.find(':');
            const std::string ns = key.substr(0, colon);
            const std::string path = key.substr(colon + 1);
            const std::filesystem::path file = DataRoot() / ns / "trial_spawner" / (path + ".json");

            std::shared_ptr<const TrialSpawnerConfig> result;
            std::ifstream in(file);
            if (!in) {
                Log::Warning("[TrialSpawner] no trial spawner config '%s' (%s)", key.c_str(), file.string().c_str());
            } else {
                try {
                    Json j;
                    in >> j;
                    TrialSpawnerConfig config;
                    std::string error;
                    if (ParseConfig(j, config, error)) {
                        result = std::make_shared<const TrialSpawnerConfig>(std::move(config));
                    } else {
                        Log::Warning("[TrialSpawner] %s: %s", file.string().c_str(), error.c_str());
                    }
                } catch (const std::exception& e) {
                    Log::Warning("[TrialSpawner] %s: %s", file.string().c_str(), e.what());
                }
            }
            g_configs[key] = result;
            return result;
        }

        bool ParseJson(const std::string& jsonText, TrialSpawnerConfig& out, std::string& error) {
            try {
                return ParseConfig(Json::parse(jsonText), out, error);
            } catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }

        SpawnData BareSpawnData(EntityTypeId type) {
            SpawnData data;
            Nbt::Writer w;
            w.BeginRootCompound();
            w.String("id", "minecraft:" + std::string(GetEntityTypeInfo(type).slug));
            w.EndRootCompound();
            if (w.ok()) data.entityNbt = w.TakeBytes();
            data.hasType = true;
            data.type = type;
            data.bareId = true;
            return data;
        }

        bool EntityTypeFromId(const std::string& id, EntityTypeId& out) {
            static const std::unordered_map<std::string, EntityTypeId> index = [] {
                std::unordered_map<std::string, EntityTypeId> map;
                for (uint16_t i = 0; i < static_cast<uint16_t>(EntityTypeId::Count); ++i) {
                    const auto type = static_cast<EntityTypeId>(i);
                    const std::string_view slug = GetEntityTypeInfo(type).slug;
                    if (!slug.empty()) map.emplace(std::string(slug), type);
                }
                return map;
            }();
            // Namespace-blind, as the entity loader is (EntityNbt's
            // EntityTypeFromName): the engine's own mobs are saved under
            // "minecraft:" too.
            const auto it = index.find(std::string(StripNamespace(id)));
            if (it == index.end()) return false;
            out = it->second;
            return true;
        }

        void Reload() {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_configs.clear();
        }

    } // namespace TrialSpawnerConfigs

} // namespace Game
