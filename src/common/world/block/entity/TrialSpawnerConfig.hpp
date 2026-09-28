// File: src/common/world/block/entity/TrialSpawnerConfig.hpp
//
// MC net.minecraft.world.level.block.entity.trialspawner.TrialSpawnerConfig
// (the record and its codec), TrialSpawner.FullConfig, and the
// TRIAL_SPAWNER_CONFIG registry the trial chambers' spawners name
// ("minecraft:trial_chamber/breeze/normal", …).
//
// The registry is DATA: data/<ns>/trial_spawner/<path>.json, the 26.3
// datapack files TrialSpawnerConfigs.bootstrap generates. They are read on
// first use and cached, the way the loot tables are (ChestLootTables.hpp) —
// nothing about a config is compiled in beyond the codec's defaults.
//
// A holder is either a registry REFERENCE (the id string a structure
// template's spawner carries) or a DIRECT value (an inline compound, or the
// config a spawn egg writes — TrialSpawnerConfig.withSpawning). Both are
// kept in the form they arrived in so a save writes the same thing back.
//
// SpawnData here is the monster spawner's (SpawnerBlockEntity.hpp): the
// entity compound as binary NBT plus the parsed id / baby / Pos fields,
// custom_spawn_rules and the equipment table.
#pragma once

#include "SpawnerBlockEntity.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Game {

    class JavaRandom;

    // MC WeightedList<ResourceKey<LootTable>> element.
    struct WeightedLootTable {
        std::string table;   // "minecraft:spawners/trial_chamber/key"
        int         weight = 1;
    };

    // MC TrialSpawnerConfig. Field defaults are TrialSpawnerConfig.Builder's.
    struct TrialSpawnerConfig {
        int   spawnRange = 4;
        float totalMobs = 6.0f;
        float simultaneousMobs = 2.0f;
        float totalMobsAddedPerPlayer = 2.0f;
        float simultaneousMobsAddedPerPlayer = 1.0f;
        int   ticksBetweenSpawn = 40;
        std::vector<WeightedSpawnData> spawnPotentials;
        std::vector<WeightedLootTable> lootTablesToEject = {
            { "minecraft:spawners/trial_chamber/consumables", 1 },
            { "minecraft:spawners/trial_chamber/key", 1 },
        };
        std::string itemsToDropWhenOminous = "minecraft:spawners/trial_chamber/items_to_drop_when_ominous";

        // MC ticksBetweenItemSpawners — a constant, not a codec field.
        static constexpr int64_t kTicksBetweenItemSpawners = 160;

        // MC calculateTargetTotalMobs / calculateTargetSimultaneousMobs:
        // floor(base + perPlayer * additionalPlayers), in float then double
        // exactly as Java widens it.
        int CalculateTargetTotalMobs(int additionalPlayers) const;
        int CalculateTargetSimultaneousMobs(int additionalPlayers) const;

        // MC WeightedList.getRandom over the potentials: an index, or -1 when
        // the list weighs nothing (Optional.empty — no draw is made then).
        int PickSpawnPotential(JavaRandom& random) const;
        // The same over loot_tables_to_eject: a table key, or "" for none.
        std::string PickLootTableToEject(JavaRandom& random) const;
    };

    // MC Holder<TrialSpawnerConfig>.
    struct TrialSpawnerConfigHolder {
        // Non-empty for a registry reference ("minecraft:trial_chamber/breeze/normal").
        std::string key;
        // Always set once built through the factories below; a reference to a
        // key the data pack lacks resolves to the codec DEFAULT (and says so
        // once in the log) — MC would refuse the block entity's config and
        // fall back to FullConfig.DEFAULT, which is the same config.
        std::shared_ptr<const TrialSpawnerConfig> value;

        const TrialSpawnerConfig& Get() const;
        bool IsReference() const { return !key.empty(); }

        static TrialSpawnerConfigHolder Default();
        static TrialSpawnerConfigHolder Reference(const std::string& key);
        static TrialSpawnerConfigHolder Direct(TrialSpawnerConfig config);
    };

    // MC TrialSpawner.FullConfig — the two holders, the 30-minute cooldown
    // and the 14-block detection range, each optional in the codec.
    struct TrialSpawnerFullConfig {
        static constexpr int kDefaultTargetCooldownLength = 36000;
        static constexpr int kDefaultRequiredPlayerRange  = 14;

        TrialSpawnerConfigHolder normal  = TrialSpawnerConfigHolder::Default();
        TrialSpawnerConfigHolder ominous = TrialSpawnerConfigHolder::Default();
        int targetCooldownLength = kDefaultTargetCooldownLength;
        int requiredPlayerRange  = kDefaultRequiredPlayerRange;

        // MC FullConfig.overrideEntity(type) — both configs become direct
        // copies spawning a bare {id} (TrialSpawnerConfig.withSpawning).
        TrialSpawnerFullConfig OverrideEntity(EntityTypeId type) const;
    };

    namespace TrialSpawnerConfigs {

        // The registry entry for `key` ("minecraft:trial_chamber/breeze/normal"
        // or a bare path), read from data/<ns>/trial_spawner/<path>.json on
        // first use. Null when the file is missing or does not decode.
        std::shared_ptr<const TrialSpawnerConfig> Lookup(const std::string& key);

        // MC TrialSpawnerConfig.DIRECT_CODEC over JSON text (a datapack file,
        // or an inline config the NBT reader converted). False, with a reason,
        // when the value does not decode — the codec's validation included
        // (spawn_range 1..128, the non-negative floats, a SpawnData's
        // light limits in 0..15).
        bool ParseJson(const std::string& jsonText, TrialSpawnerConfig& out, std::string& error);

        // A bare {id:"minecraft:<slug>"} SpawnData — MC's `new SpawnData(tag,
        // empty, empty)` after putString("id"), the form withSpawning builds.
        SpawnData BareSpawnData(EntityTypeId type);

        // The entity type named by an "id" string ("minecraft:zombie" or
        // "zombie"). False when this build has no such type.
        bool EntityTypeFromId(const std::string& id, EntityTypeId& out);

        // Forget every cached entry; the next Lookup re-reads the data pack.
        void Reload();

    } // namespace TrialSpawnerConfigs

} // namespace Game
