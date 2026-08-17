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
    };

    static_assert(sizeof(kMobCategoryTable) / sizeof(kMobCategoryTable[0]) ==
                      static_cast<size_t>(MobCategory::Count),
                  "kMobCategoryTable must stay in sync with MobCategory");

    inline const MobCategoryInfo& GetMobCategoryInfo(MobCategory c) {
        return kMobCategoryTable[static_cast<size_t>(c)];
    }

} // namespace Game
