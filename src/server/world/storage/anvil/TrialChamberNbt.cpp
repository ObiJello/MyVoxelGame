// File: src/server/world/storage/anvil/TrialChamberNbt.cpp
#include "server/world/storage/anvil/TrialChamberNbt.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/world/storage/anvil/SpawnerNbt.hpp"

#include "common/core/Log.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/world/block/entity/TrialSpawnerBlockEntity.hpp"
#include "common/world/block/entity/VaultBlockEntity.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace Game::Anvil {

    namespace {

        using ::World::NBTTag;
        using ::World::NBTTagCompound;
        using ::World::NBTTagList;
        using ::World::NBTTagPtr;
        using ::World::NBTTagType;

        std::string WithNamespace(std::string id) {
            if (!id.empty() && id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        bool IsNumeric(const NBTTagPtr& t) {
            if (!t) return false;
            switch (t->type) {
                case NBTTagType::TAG_Byte: case NBTTagType::TAG_Short: case NBTTagType::TAG_Int:
                case NBTTagType::TAG_Long: case NBTTagType::TAG_Float: case NBTTagType::TAG_Double:
                    return true;
                default:
                    return false;
            }
        }

        // ── UUIDs (UUIDUtil.CODEC: an int array of four) ────────────────────

        bool ReadUuid(const NBTTagPtr& tag, Uuid& out) {
            if (auto arr = std::dynamic_pointer_cast<::World::NBTTagIntArray>(tag); arr && arr->value.size() == 4) {
                const int32_t v[4] = { arr->value[0], arr->value[1], arr->value[2], arr->value[3] };
                out = UuidFromIntArray(v);
                return true;
            }
            return false;
        }

        // UUIDUtil.CODEC_SET / CODEC_LINKED_SET. Read leniently (MC's
        // lenientOptionalFieldOf: a malformed list reads as the default,
        // empty); duplicates collapse as the set would.
        std::vector<Uuid> ReadUuidSet(const NBTTagCompound& tag, const char* key) {
            std::vector<Uuid> out;
            auto list = std::dynamic_pointer_cast<NBTTagList>(tag.GetTag(key));
            if (!list) return out;
            for (const NBTTagPtr& element : list->value) {
                Uuid id{};
                if (!ReadUuid(element, id)) return {};
                if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
            }
            return out;
        }

        void WriteUuidSet(Nbt::Writer& w, const char* key, const std::vector<Uuid>& set) {
            if (set.empty()) return;   // the default, Set.of()
            auto list = w.BeginList(key, Nbt::TagType::IntArray);
            for (const Uuid& id : set) {
                int32_t v[4];
                UuidToIntArray(id, v);
                w.ListIntArray(list, v, 4);
            }
            w.EndList(list);
        }

        // ── TrialSpawnerConfig.DIRECT_CODEC over NBT ────────────────────────

        bool ReadIntIn(const NBTTagCompound& tag, const char* key, int lo, int hi, int& out) {
            NBTTagPtr t = tag.GetTag(key);
            if (!t) return true;
            if (!IsNumeric(t)) return false;
            const int64_t v = tag.GetValue<int64_t>(key, 0);
            if (v < lo || v > hi) return false;
            out = static_cast<int>(v);
            return true;
        }

        bool ReadNonNegativeFloat(const NBTTagCompound& tag, const char* key, float& out) {
            NBTTagPtr t = tag.GetTag(key);
            if (!t) return true;
            if (!IsNumeric(t)) return false;
            const float v = tag.GetValue<float>(key, 0.0f);
            if (!(v >= 0.0f)) return false;
            out = v;
            return true;
        }

        // WeightedList.codec: [{data, weight}] with a non-negative int weight.
        template <class ReadData>
        bool ReadWeightedList(const NBTTagCompound& tag, const char* key, ReadData&& readData) {
            NBTTagPtr t = tag.GetTag(key);
            if (!t) return true;
            auto list = std::dynamic_pointer_cast<NBTTagList>(t);
            if (!list) return false;
            for (const NBTTagPtr& element : list->value) {
                auto entry = std::dynamic_pointer_cast<NBTTagCompound>(element);
                if (!entry || !entry->HasTag("data") || !IsNumeric(entry->GetTag("weight"))) return false;
                const int64_t weight = entry->GetValue<int64_t>("weight", -1);
                if (weight < 0 || weight > std::numeric_limits<int32_t>::max()) return false;
                if (!readData(entry->GetTag("data"), static_cast<int>(weight))) return false;
            }
            return true;
        }

        bool ReadDirectConfig(const NBTTagCompound& tag, TrialSpawnerConfig& out) {
            out = TrialSpawnerConfig{};
            if (!ReadIntIn(tag, "spawn_range", 1, 128, out.spawnRange)) return false;
            if (!ReadNonNegativeFloat(tag, "total_mobs", out.totalMobs)) return false;
            if (!ReadNonNegativeFloat(tag, "simultaneous_mobs", out.simultaneousMobs)) return false;
            if (!ReadNonNegativeFloat(tag, "total_mobs_added_per_player", out.totalMobsAddedPerPlayer)) return false;
            if (!ReadNonNegativeFloat(tag, "simultaneous_mobs_added_per_player",
                                      out.simultaneousMobsAddedPerPlayer)) return false;
            if (!ReadIntIn(tag, "ticks_between_spawn", 0, std::numeric_limits<int32_t>::max(),
                           out.ticksBetweenSpawn)) return false;
            if (tag.HasTag("spawn_potentials")) {
                std::vector<WeightedSpawnData> potentials;
                const bool ok = ReadWeightedList(tag, "spawn_potentials", [&](const NBTTagPtr& data, int weight) {
                    auto compound = std::dynamic_pointer_cast<NBTTagCompound>(data);
                    WeightedSpawnData w;
                    w.weight = weight;
                    if (!compound || !ReadSpawnDataTag(*compound, w.data)) return false;
                    potentials.push_back(std::move(w));
                    return true;
                });
                if (!ok) return false;
                out.spawnPotentials = std::move(potentials);
            }
            if (tag.HasTag("loot_tables_to_eject")) {
                std::vector<WeightedLootTable> tables;
                const bool ok = ReadWeightedList(tag, "loot_tables_to_eject", [&](const NBTTagPtr& data, int weight) {
                    auto key = std::dynamic_pointer_cast<::World::NBTTagString>(data);
                    if (!key || key->value.empty()) return false;
                    tables.push_back({ WithNamespace(key->value), weight });
                    return true;
                });
                if (!ok) return false;
                out.lootTablesToEject = std::move(tables);
            }
            if (NBTTagPtr t = tag.GetTag("items_to_drop_when_ominous")) {
                auto key = std::dynamic_pointer_cast<::World::NBTTagString>(t);
                if (!key || key->value.empty()) return false;
                out.itemsToDropWhenOminous = WithNamespace(key->value);
            }
            return true;
        }

        void WriteDirectConfigBody(Nbt::Writer& w, const TrialSpawnerConfig& c) {
            // optionalFieldOf(name, DEFAULT.x): a value at its default is not
            // written.
            const TrialSpawnerConfig def;
            if (c.spawnRange != def.spawnRange) w.Int("spawn_range", c.spawnRange);
            if (c.totalMobs != def.totalMobs) w.Float("total_mobs", c.totalMobs);
            if (c.simultaneousMobs != def.simultaneousMobs) w.Float("simultaneous_mobs", c.simultaneousMobs);
            if (c.totalMobsAddedPerPlayer != def.totalMobsAddedPerPlayer) {
                w.Float("total_mobs_added_per_player", c.totalMobsAddedPerPlayer);
            }
            if (c.simultaneousMobsAddedPerPlayer != def.simultaneousMobsAddedPerPlayer) {
                w.Float("simultaneous_mobs_added_per_player", c.simultaneousMobsAddedPerPlayer);
            }
            if (c.ticksBetweenSpawn != def.ticksBetweenSpawn) w.Int("ticks_between_spawn", c.ticksBetweenSpawn);
            if (!c.spawnPotentials.empty()) {
                auto list = w.BeginList("spawn_potentials", Nbt::TagType::Compound);
                for (const WeightedSpawnData& weighted : c.spawnPotentials) {
                    w.ListCompoundBegin(list);
                    w.BeginCompound("data");
                    WriteSpawnDataBody(w, weighted.data);
                    w.EndCompound();
                    w.Int("weight", weighted.weight);
                    w.ListCompoundEnd(list);
                }
                w.EndList(list);
            }
            bool defaultTables = c.lootTablesToEject.size() == def.lootTablesToEject.size();
            for (size_t i = 0; defaultTables && i < c.lootTablesToEject.size(); ++i) {
                defaultTables = c.lootTablesToEject[i].table == def.lootTablesToEject[i].table &&
                                c.lootTablesToEject[i].weight == def.lootTablesToEject[i].weight;
            }
            if (!defaultTables) {
                auto list = w.BeginList("loot_tables_to_eject", Nbt::TagType::Compound);
                for (const WeightedLootTable& t : c.lootTablesToEject) {
                    w.ListCompoundBegin(list);
                    w.String("data", t.table);
                    w.Int("weight", t.weight);
                    w.ListCompoundEnd(list);
                }
                w.EndList(list);
            }
            if (c.itemsToDropWhenOminous != def.itemsToDropWhenOminous) {
                w.String("items_to_drop_when_ominous", c.itemsToDropWhenOminous);
            }
        }

        // TrialSpawnerConfig.CODEC (RegistryCodecs.holder): a registry id or
        // a direct compound. False when it does not decode.
        bool ReadConfigHolder(const NBTTagPtr& tag, TrialSpawnerConfigHolder& out) {
            if (auto key = std::dynamic_pointer_cast<::World::NBTTagString>(tag)) {
                if (key->value.empty()) return false;
                if (!TrialSpawnerConfigs::Lookup(key->value)) return false;   // unknown registry entry
                out = TrialSpawnerConfigHolder::Reference(key->value);
                return true;
            }
            if (auto compound = std::dynamic_pointer_cast<NBTTagCompound>(tag)) {
                TrialSpawnerConfig config;
                if (!ReadDirectConfig(*compound, config)) return false;
                out = TrialSpawnerConfigHolder::Direct(std::move(config));
                return true;
            }
            return false;
        }

        void WriteConfigHolder(Nbt::Writer& w, const char* key, const TrialSpawnerConfigHolder& holder) {
            if (holder.IsReference()) {
                w.String(key, holder.key);
                return;
            }
            w.BeginCompound(key);
            WriteDirectConfigBody(w, holder.Get());
            w.EndCompound();
        }

        // A holder equal to FullConfig's default: the direct DEFAULT config.
        bool IsDefaultHolder(const TrialSpawnerConfigHolder& holder) {
            if (holder.IsReference()) return false;
            const TrialSpawnerConfig& c = holder.Get();
            const TrialSpawnerConfig def;
            if (c.spawnRange != def.spawnRange || c.totalMobs != def.totalMobs ||
                c.simultaneousMobs != def.simultaneousMobs ||
                c.totalMobsAddedPerPlayer != def.totalMobsAddedPerPlayer ||
                c.simultaneousMobsAddedPerPlayer != def.simultaneousMobsAddedPerPlayer ||
                c.ticksBetweenSpawn != def.ticksBetweenSpawn || !c.spawnPotentials.empty() ||
                c.itemsToDropWhenOminous != def.itemsToDropWhenOminous ||
                c.lootTablesToEject.size() != def.lootTablesToEject.size()) {
                return false;
            }
            for (size_t i = 0; i < c.lootTablesToEject.size(); ++i) {
                if (c.lootTablesToEject[i].table != def.lootTablesToEject[i].table ||
                    c.lootTablesToEject[i].weight != def.lootTablesToEject[i].weight) {
                    return false;
                }
            }
            return true;
        }

        // ── Vault codecs ────────────────────────────────────────────────────

        // VaultConfig.CODEC. False when it does not decode (the caller falls
        // back to VaultConfig.DEFAULT, as `read("config").orElse(DEFAULT)`).
        bool ReadVaultConfig(const NBTTagCompound& tag, VaultConfig& out) {
            out = VaultConfig{};
            if (NBTTagPtr t = tag.GetTag("loot_table")) {
                auto key = std::dynamic_pointer_cast<::World::NBTTagString>(t);
                if (key && !key->value.empty()) out.lootTable = WithNamespace(key->value);
            }
            if (IsNumeric(tag.GetTag("activation_range"))) {
                out.activationRange = tag.GetValue<double>("activation_range", out.activationRange);
            }
            if (IsNumeric(tag.GetTag("deactivation_range"))) {
                out.deactivationRange = tag.GetValue<double>("deactivation_range", out.deactivationRange);
            }
            // ItemStack.lenientOptionalFieldOf("key_item"): absent (or bad)
            // is EMPTY — NOT the default trial key. A vault without a key
            // never opens.
            out.keyItem = ItemStack{};
            if (auto key = std::dynamic_pointer_cast<NBTTagCompound>(tag.GetTag("key_item"))) {
                out.keyItem = ReadItemStack(*key);
            }
            if (auto display = std::dynamic_pointer_cast<::World::NBTTagString>(
                    tag.GetTag("override_loot_table_to_display"));
                display && !display->value.empty()) {
                out.overrideLootTableToDisplay = WithNamespace(display->value);
            }
            // VaultConfig.validate.
            return out.activationRange <= out.deactivationRange;
        }

        void WriteVaultConfigBody(Nbt::Writer& w, const VaultConfig& c) {
            if (c.lootTable != VaultConfig::kDefaultLootTable) w.String("loot_table", c.lootTable);
            if (c.activationRange != VaultConfig::kDefaultActivationRange) {
                w.Double("activation_range", c.activationRange);
            }
            if (c.deactivationRange != VaultConfig::kDefaultDeactivationRange) {
                w.Double("deactivation_range", c.deactivationRange);
            }
            if (!c.keyItem.IsEmpty()) {
                w.BeginCompound("key_item");
                WriteItemStackBody(w, c.keyItem);
                w.EndCompound();
            }
            if (c.overrideLootTableToDisplay) {
                w.String("override_loot_table_to_display", *c.overrideLootTableToDisplay);
            }
        }

        std::vector<ItemStack> ReadItemList(const NBTTagCompound& tag, const char* key) {
            std::vector<ItemStack> out;
            auto list = std::dynamic_pointer_cast<NBTTagList>(tag.GetTag(key));
            if (!list) return out;
            for (const NBTTagPtr& element : list->value) {
                auto compound = std::dynamic_pointer_cast<NBTTagCompound>(element);
                if (!compound) return {};
                ItemStack stack = ReadItemStack(*compound);
                if (stack.IsEmpty()) return {};   // ItemStack.CODEC refuses an empty stack
                out.push_back(std::move(stack));
            }
            return out;
        }

    } // namespace

    // ── Trial spawner ───────────────────────────────────────────────────────

    void ReadTrialSpawner(const NBTTagCompound& tag, TrialSpawnerBlockEntity& spawner) {
        // TrialSpawnerStateData.Packed (every field lenient).
        TrialSpawnerBlockEntity::StateData data;
        data.detectedPlayers = ReadUuidSet(tag, "registered_players");
        data.currentMobs = ReadUuidSet(tag, "current_mobs");
        if (IsNumeric(tag.GetTag("cooldown_ends_at"))) data.cooldownEndsAt = tag.GetValue<int64_t>("cooldown_ends_at", 0);
        if (IsNumeric(tag.GetTag("next_mob_spawns_at"))) {
            data.nextMobSpawnsAt = tag.GetValue<int64_t>("next_mob_spawns_at", 0);
        }
        if (IsNumeric(tag.GetTag("total_mobs_spawned"))) {
            data.totalMobsSpawned = std::max(0, tag.GetValue<int32_t>("total_mobs_spawned", 0));
        }
        if (auto spawnData = std::dynamic_pointer_cast<NBTTagCompound>(tag.GetTag("spawn_data"))) {
            SpawnData next;
            if (ReadSpawnDataTag(*spawnData, next)) data.nextSpawnData = std::move(next);
        }
        if (auto table = std::dynamic_pointer_cast<::World::NBTTagString>(tag.GetTag("ejecting_loot_table"));
            table && !table->value.empty()) {
            data.ejectingLootTable = WithNamespace(table->value);
        }
        spawner.SetStateData(std::move(data));

        // TrialSpawner.FullConfig.MAP_CODEC — any field that fails makes the
        // whole config the DEFAULT (read(...).orElse(DEFAULT)).
        TrialSpawnerFullConfig config;
        bool ok = true;
        if (NBTTagPtr normal = tag.GetTag("normal_config")) ok = ok && ReadConfigHolder(normal, config.normal);
        if (NBTTagPtr ominous = tag.GetTag("ominous_config")) ok = ok && ReadConfigHolder(ominous, config.ominous);
        if (NBTTagPtr cooldown = tag.GetTag("target_cooldown_length")) {
            const int64_t v = tag.GetValue<int64_t>("target_cooldown_length", -1);
            if (!IsNumeric(cooldown) || v < 0 || v > std::numeric_limits<int32_t>::max()) ok = false;
            else config.targetCooldownLength = static_cast<int>(v);
        }
        if (NBTTagPtr range = tag.GetTag("required_player_range")) {
            const int64_t v = tag.GetValue<int64_t>("required_player_range", 0);
            if (!IsNumeric(range) || v < 1 || v > 128) ok = false;
            else config.requiredPlayerRange = static_cast<int>(v);
        }
        if (!ok) {
            Log::Warning("[TrialSpawner] config does not decode; using the default");
            config = TrialSpawnerFullConfig{};
        }
        spawner.SetFullConfig(std::move(config));
    }

    void WriteTrialSpawner(Nbt::Writer& w, const TrialSpawnerBlockEntity& spawner) {
        const TrialSpawnerBlockEntity::StateData& data = spawner.GetStateData();
        WriteUuidSet(w, "registered_players", data.detectedPlayers);
        WriteUuidSet(w, "current_mobs", data.currentMobs);
        if (data.cooldownEndsAt != 0) w.Long("cooldown_ends_at", data.cooldownEndsAt);
        if (data.nextMobSpawnsAt != 0) w.Long("next_mob_spawns_at", data.nextMobSpawnsAt);
        if (data.totalMobsSpawned != 0) w.Int("total_mobs_spawned", data.totalMobsSpawned);
        if (data.nextSpawnData) {
            w.BeginCompound("spawn_data");
            WriteSpawnDataBody(w, *data.nextSpawnData);
            w.EndCompound();
        }
        if (data.ejectingLootTable) w.String("ejecting_loot_table", *data.ejectingLootTable);

        const TrialSpawnerFullConfig& config = spawner.GetFullConfig();
        if (!IsDefaultHolder(config.normal)) WriteConfigHolder(w, "normal_config", config.normal);
        if (!IsDefaultHolder(config.ominous)) WriteConfigHolder(w, "ominous_config", config.ominous);
        if (config.targetCooldownLength != TrialSpawnerFullConfig::kDefaultTargetCooldownLength) {
            w.Int("target_cooldown_length", config.targetCooldownLength);
        }
        if (config.requiredPlayerRange != TrialSpawnerFullConfig::kDefaultRequiredPlayerRange) {
            w.Int("required_player_range", config.requiredPlayerRange);
        }
    }

    // ── Vault ───────────────────────────────────────────────────────────────

    void ReadVault(const NBTTagCompound& tag, VaultBlockEntity& vault) {
        // MC loadAdditional's order: server_data, config, shared_data.
        if (auto server = std::dynamic_pointer_cast<NBTTagCompound>(tag.GetTag("server_data"))) {
            VaultServerData data;
            data.rewardedPlayers = ReadUuidSet(*server, "rewarded_players");
            if (IsNumeric(server->GetTag("state_updating_resumes_at"))) {
                data.stateUpdatingResumesAt = server->GetValue<int64_t>("state_updating_resumes_at", 0);
            }
            data.itemsToEject = ReadItemList(*server, "items_to_eject");
            if (IsNumeric(server->GetTag("total_ejections_needed"))) {
                data.totalEjectionsNeeded = server->GetValue<int32_t>("total_ejections_needed", 0);
            }
            vault.LoadServerData(data);
        }

        VaultConfig config;
        if (auto c = std::dynamic_pointer_cast<NBTTagCompound>(tag.GetTag("config"))) {
            if (!ReadVaultConfig(*c, config)) {
                Log::Warning("[Vault] config does not decode; using the default");
                config = VaultConfig{};
            }
        }
        vault.SetConfig(std::move(config));

        if (auto shared = std::dynamic_pointer_cast<NBTTagCompound>(tag.GetTag("shared_data"))) {
            VaultSharedData data;
            if (auto display = std::dynamic_pointer_cast<NBTTagCompound>(shared->GetTag("display_item"))) {
                data.displayItem = ReadItemStack(*display);
            }
            data.connectedPlayers = ReadUuidSet(*shared, "connected_players");
            if (IsNumeric(shared->GetTag("connected_particles_range"))) {
                data.connectedParticlesRange = shared->GetValue<double>("connected_particles_range",
                                                                        data.connectedParticlesRange);
            }
            vault.LoadSharedData(data);
        }
    }

    void WriteVault(Nbt::Writer& w, const VaultBlockEntity& vault) {
        w.BeginCompound("config");
        WriteVaultConfigBody(w, vault.GetConfig());
        w.EndCompound();

        const VaultSharedData& shared = vault.GetSharedData();
        w.BeginCompound("shared_data");
        if (!shared.displayItem.IsEmpty()) {
            w.BeginCompound("display_item");
            WriteItemStackBody(w, shared.displayItem);
            w.EndCompound();
        }
        WriteUuidSet(w, "connected_players", shared.connectedPlayers);
        if (shared.connectedParticlesRange != VaultConfig::kDefaultDeactivationRange) {
            w.Double("connected_particles_range", shared.connectedParticlesRange);
        }
        w.EndCompound();

        const VaultServerData& server = vault.GetServerData();
        w.BeginCompound("server_data");
        WriteUuidSet(w, "rewarded_players", server.rewardedPlayers);
        if (server.stateUpdatingResumesAt != 0) w.Long("state_updating_resumes_at", server.stateUpdatingResumesAt);
        if (!server.itemsToEject.empty()) {
            auto list = w.BeginList("items_to_eject", Nbt::TagType::Compound);
            for (const ItemStack& stack : server.itemsToEject) {
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, stack);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }
        if (server.totalEjectionsNeeded != 0) w.Int("total_ejections_needed", server.totalEjectionsNeeded);
        w.EndCompound();
    }

    // ── Hooks ───────────────────────────────────────────────────────────────

    namespace {
        bool CheckTrialSpawnerSpawnRules(EntityTypeId type, ::Game::World& world, const glm::ivec3& pos,
                                         JavaRandom& random) {
            return CheckSpawnRulesAt(type, world, pos, random, SpawnReason::TrialSpawner);
        }
    } // namespace

    void InstallTrialChamberServerHooks() {
        TrialSpawnerServerHooks hooks;
        hooks.checkSpawnRules = &CheckTrialSpawnerSpawnRules;
        SetTrialSpawnerServerHooks(hooks);
    }

} // namespace Game::Anvil
