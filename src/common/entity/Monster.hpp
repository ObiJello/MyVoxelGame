// File: src/common/entity/Monster.hpp
//
// MC net.minecraft.world.entity.monster.Monster — a hostile PathfinderMob.
//
// Two behaviours live here and nowhere else:
//
//  * `GetWalkTargetValue` returns the NEGATED light cost, so monsters actively
//    prefer dark positions when wandering. Animals return the positive value.
//    Getting the sign wrong makes zombies wander into the sun.
//
//  * `UpdateNoActionTime` adds an EXTRA +2 per tick in light. noActionTime
//    drives despawning, so a monster standing in daylight ages toward despawn
//    three times faster than one in a cave — which is a large part of why lit
//    areas stay clear.
#pragma once

#include "common/entity/Mob.hpp"
#include "common/entity/SpawnReason.hpp"

namespace Game {

    class JavaRandom;

    class Monster : public PathfinderMob {
    public:
        Monster(EntityTypeId type, EntityLevel* level);

        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        // MC Monster.aiStep prepends the swing timer and the light penalty.
        void AiStep() override;

        // MC Monster.shouldDropLoot — monsters drop even as babies, unlike
        // animals.
        virtual bool ShouldDropLoot() const { return true; }

        // XP comes from Mob::GetXpReward — the type table already carries the
        // per-monster ctor values (Monster.java:34's base 5 plus overrides).

        // ── Spawn rules (MC Monster.checkMonsterSpawnRules family) ─────────
        //
        // Static because the spawner asks before an entity exists. `rng` is
        // MC's `random` parameter — the spawner passes the level's random, so
        // the rolls stay on the shared stream.
        static bool CheckMonsterSpawnRules(EntityLevel& level, SpawnReason reason,
                                           const glm::ivec3& pos, JavaRandom& rng);
        // MC Monster.checkAnyLightMonsterSpawnRules — blaze/breeze/zoglin and
        // the endermite/silverfish base: peaceful and surface checks only, no
        // light test.
        static bool CheckAnyLightMonsterSpawnRules(EntityLevel& level, SpawnReason reason,
                                                   const glm::ivec3& pos, JavaRandom& rng);
        // MC Monster.checkSurfaceMonstersSpawnRules — husk/parched/camel husk:
        // monster rules plus open sky.
        static bool CheckSurfaceMonstersSpawnRules(EntityLevel& level, SpawnReason reason,
                                                   const glm::ivec3& pos, JavaRandom& rng);
        static bool IsDarkEnoughToSpawn(EntityLevel& level, const glm::ivec3& pos,
                                        JavaRandom& rng);

    protected:
        void UpdateNoActionTime();
    };

} // namespace Game
