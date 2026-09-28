// File: src/common/entity/SpawnReason.hpp
//
// MC net.minecraft.world.entity.EntitySpawnReason, reduced to the values this
// engine can produce. The spawn-rule predicates branch on these — a drowned
// from a spawner skips its 1-in-40 roll, a slime from a spawner skips the
// slime-chunk test — so the reason has to travel with every rules check even
// though most paths today are Natural.
#pragma once

#include <cstdint>

namespace Game {

    enum class SpawnReason : uint8_t {
        Natural,
        ChunkGeneration,
        Spawner,          // monster-spawner block (BaseSpawner)
        Breeding,
        MobSummoned,      // one mob creating another (zombie reinforcements)
        Jockey,
        Conversion,
        Reinforcement,
        SpawnItemUse,     // spawn egg
        Command,          // /summon
        Load,
        Triggered,        // block-pattern completion (wither ritual, golems)
        // MC EntitySpawnReason.STRUCTURE — mobs placed by world generation
        // (template entities, swamp-hut witch, monument elders, mansion
        // illagers). Piglins skip their baby/weapon roll for it, and raiders
        // never become patrol leaders from it.
        Structure,
        // MC EntitySpawnReason.EVENT — spawned by a CustomSpawner's event:
        // the wandering trader and its llamas (WanderingTraderSpawner).
        Event,
        // MC EntitySpawnReason.BUCKET — released from a mob bucket
        // (MobBucketItem.spawn). The axolotl skips its variant/baby roll for
        // it; the bucket data sets what the mob was.
        Bucket,
        // MC EntitySpawnReason.TRIAL_SPAWNER — a trial spawner's wave
        // (TrialSpawnerBlockEntity). A spawner for isSpawner, and the one
        // reason that ignores light requirements.
        TrialSpawner,
        // MC EntitySpawnReason.PATROL — a pillager patrol member
        // (PatrolSpawner). PatrollingMonster.finalizeSpawn marks the mob
        // patrolling and never rolls it a leader for it.
        Patrol,
    };

    // MC EntitySpawnReason.isSpawner — SPAWNER || TRIAL_SPAWNER.
    inline bool IsSpawner(SpawnReason r) {
        return r == SpawnReason::Spawner || r == SpawnReason::TrialSpawner;
    }

    // MC EntitySpawnReason.ignoresLightRequirements — TRIAL_SPAWNER only: a
    // trial chamber's lit corridors still spawn their waves.
    inline bool IgnoresLightRequirements(SpawnReason r) { return r == SpawnReason::TrialSpawner; }

} // namespace Game
