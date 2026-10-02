// File: src/common/world/spawn/MobCapReport.hpp
//
// A read-only picture of one level's natural-spawning budget — MC
// NaturalSpawner's global caps and LocalMobCapCalculator's per-player caps —
// for the ImGui "Mob Caps" debug panel.
//
// Built on the server thread (IntegratedServer::BuildMobCapReport, about once
// a second while the panel is open) with the SAME census rules the spawner
// uses, then copied out under a mutex. Plain data only: DebugSystem.cpp is in
// the `imgui` target and must not see any server header.
#pragma once

#include "common/entity/MobCategory.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    struct MobCapReport {
        // One entity type's mobs in a category, split by how the census
        // treated them.
        struct TypeRow {
            std::string name;            // entity type slug
            int counted = 0;             // charged to the cap
            int countedNamed = 0;        // ...of which carry a custom name (MC still counts them)
            int persistent = 0;          // excluded: PersistenceRequired (name tag, picked up items, ...)
            int leashed = 0;             // excluded: RequiresCustomPersistence — on a lead
            int passenger = 0;           // excluded: RequiresCustomPersistence — riding something
            int otherCustom = 0;         // excluded: RequiresCustomPersistence — bucket fish, etc.
            int tamed = 0;               // excluded: tamed pet (deliberate engine deviation)
            int Total() const { return counted + persistent + leashed + passenger + otherCustom + tamed; }
        };

        struct CategoryRow {
            MobCategory category = MobCategory::Monster;
            const char* name = "";       // MC's serialized name ("monster", ...)
            int count = 0;               // census count (what the cap compares)
            int globalCap = 0;           // max * spawnableChunks / 289
            int rawCap = 0;              // maxInstancesPerChunk (the local cap)
            int excludedPersistent = 0;  // persistent + leashed + passenger + otherCustom
            int excludedTamed = 0;
            bool enabled = false;        // spawning of this category is allowed at all
            const char* disabledReason = ""; // why not, when !enabled
            bool globalFull = false;     // count >= globalCap
            bool localFullEverywhere = false; // every player's local cap is full
            // CREATURE-style categories only get a pass every 400 ticks.
            bool persistentGate = false;
            int  ticksToNextPass = 0;
            std::vector<TypeRow> types;  // sorted by Total(), descending
        };

        struct PlayerRow {
            std::string name;
            std::array<int, kMobCategoryCount> localCount{};
        };

        int  dimension = 0;              // Game::DimensionId value
        std::string dimensionName;
        bool valid = false;

        int64_t gameTick = 0;
        std::chrono::steady_clock::time_point builtAt{};

        int spawnableChunkCount = 0;     // union of 17x17 squares (the cap's denominator)
        int spawningChunkCount = 0;      // chunks that actually receive spawn attempts
        int playerCount = 0;             // non-spectator players in the level
        bool doMobSpawning = true;
        bool spawnMonstersRule = true;
        bool peaceful = false;
        int totalLiveMobs = 0;           // every live mob in the level, any category

        std::vector<CategoryRow> categories;
        std::vector<PlayerRow> players;
    };

    // MC MobCategory.getName().
    inline const char* MobCategoryName(MobCategory c) {
        switch (c) {
            case MobCategory::Monster:                  return "monster";
            case MobCategory::Creature:                 return "creature";
            case MobCategory::Ambient:                  return "ambient";
            case MobCategory::Axolotls:                 return "axolotls";
            case MobCategory::UndergroundWaterCreature: return "underground_water_creature";
            case MobCategory::WaterCreature:            return "water_creature";
            case MobCategory::WaterAmbient:             return "water_ambient";
            case MobCategory::Misc:                     return "misc";
            case MobCategory::AetherSurfaceMonster:     return "aether_surface_monster";
            case MobCategory::AetherDarknessMonster:    return "aether_darkness_monster";
            case MobCategory::AetherSkyMonster:         return "aether_sky_monster";
            case MobCategory::AetherAerwhale:           return "aether_aerwhale";
            case MobCategory::Count:                    break;
        }
        return "?";
    }

} // namespace Game
