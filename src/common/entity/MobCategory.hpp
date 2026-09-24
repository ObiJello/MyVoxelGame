// File: src/common/entity/MobCategory.hpp
//
// MC net.minecraft.world.entity.MobCategory — the spawn/despawn bucket an
// entity type belongs to.
//
// These four numbers drive the whole natural-spawning budget, so they are
// worth reading carefully:
//
//   maxInstancesPerChunk  feeds the global cap, which is NOT per chunk despite
//                         the name: NaturalSpawner computes
//                         `max * spawnableChunkCount / 289`. The 289 is 17²,
//                         MC's "chunks a player keeps spawnable" square.
//   isFriendly            passives spawn on the CREATURE pass, hostiles on the
//                         MONSTER pass, and the two are gated separately.
//   isPersistent          CREATURE only spawns once every 400 ticks; the rest
//                         get a chance every tick.
//   despawnDistance       beyond this a mob is removed immediately; inside
//                         noDespawnDistance it is never removed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Game {

    enum class MobCategory : uint8_t {
        Monster,
        Creature,
        Ambient,
        Axolotls,
        UndergroundWaterCreature,
        WaterCreature,
        WaterAmbient,
        Misc,
        // The Aether's own categories (its META-INF/enumextensions.json:
        // NeoForge appends them to MC's MobCategory, after MISC — so they
        // sit after Misc here too and every vanilla ordinal stays put).
        // They take part in natural spawning like any non-MISC category
        // (NaturalSpawner's SPAWNING_CATEGORIES), each with its own cap.
        AetherSurfaceMonster,
        AetherDarknessMonster,
        AetherSkyMonster,
        AetherAerwhale,
        Count
    };

    struct MobCategoryInfo {
        int  maxInstancesPerChunk;
        bool isFriendly;
        bool isPersistent;
        int  despawnDistance;
    };

    // MobCategory.java, in enum order. noDespawnDistance is 32 for every
    // category in MC, so it is a constant rather than a column.
    inline constexpr int kNoDespawnDistance = 32;

    inline constexpr MobCategoryInfo kMobCategoryTable[] = {
        /* Monster                  */ { 70, false, false, 128 },
        /* Creature                 */ { 10, true,  true,  128 },
        /* Ambient                  */ { 15, true,  false, 128 },
        /* Axolotls                 */ {  5, true,  false, 128 },
        /* UndergroundWaterCreature */ {  5, true,  false, 128 },
        /* WaterCreature            */ {  5, true,  false, 128 },
        /* WaterAmbient             */ { 20, true,  false,  64 },
        /* Misc                     */ { -1, true,  true,  128 },
        // enumextensions.json rows: (name, max, isFriendly, isPersistent,
        // despawnDistance).
        /* AetherSurfaceMonster     */ { 15, false, false, 128 },
        /* AetherDarknessMonster    */ {  5, false, false, 128 },
        /* AetherSkyMonster         */ {  4, false, false, 128 },
        /* AetherAerwhale           */ {  1, true,  false, 128 },
    };

    static_assert(sizeof(kMobCategoryTable) / sizeof(kMobCategoryTable[0]) ==
                      static_cast<size_t>(MobCategory::Count),
                  "kMobCategoryTable must stay in sync with MobCategory");

    inline const MobCategoryInfo& GetMobCategoryInfo(MobCategory c) {
        return kMobCategoryTable[static_cast<size_t>(c)];
    }

    inline constexpr size_t kMobCategoryCount = static_cast<size_t>(MobCategory::Count);

    // "Is this a hostile mob" by category — MC's `instanceof Monster` /
    // Enemy checks, which this engine answers from the type table. MONSTER
    // plus the Aether's three monster categories (its cockatrice, zephyr,
    // swets, whirlwinds and aechor plant are Monster/Enemy classes filed
    // under their own spawn buckets).
    inline constexpr bool IsMonsterCategory(MobCategory c) {
        return c == MobCategory::Monster ||
               c == MobCategory::AetherSurfaceMonster ||
               c == MobCategory::AetherDarknessMonster ||
               c == MobCategory::AetherSkyMonster;
    }

} // namespace Game
