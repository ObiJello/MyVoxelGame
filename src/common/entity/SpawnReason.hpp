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
        Spawner,          // monster-spawner block (none exist yet; the rules honour it already)
        Breeding,
        MobSummoned,      // one mob creating another (zombie reinforcements)
        Jockey,
        Conversion,
        Reinforcement,
        SpawnItemUse,     // spawn egg
        Command,          // /summon
        Load,
        Triggered,        // block-pattern completion (wither ritual, golems)
    };

    // MC EntitySpawnReason.isSpawner — SPAWNER || TRIAL_SPAWNER.
    inline bool IsSpawner(SpawnReason r) { return r == SpawnReason::Spawner; }

    // MC EntitySpawnReason.ignoresLightRequirements — TRIAL_SPAWNER only,
    // which this engine does not have. Kept so every call site reads like the
    // vanilla line it ports.
    inline bool IgnoresLightRequirements(SpawnReason) { return false; }

} // namespace Game
