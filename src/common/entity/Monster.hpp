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

namespace Game {

    class Monster : public PathfinderMob {
    public:
        Monster(EntityTypeId type, EntityLevel* level);

        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        // MC Monster.aiStep prepends the swing timer and the light penalty.
        void AiStep() override;

        // MC Monster.shouldDropLoot — monsters drop even as babies, unlike
        // animals.
        virtual bool ShouldDropLoot() const { return true; }

        int GetXpReward() const { return TypeInfo().xpReward; }

        // ── Spawn rules (MC Monster.checkMonsterSpawnRules) ────────────────
        //
        // Static because the spawner asks before an entity exists.
        static bool CheckMonsterSpawnRules(EntityLevel& level, const glm::ivec3& pos);
        static bool IsDarkEnoughToSpawn(EntityLevel& level, const glm::ivec3& pos);

    protected:
        void UpdateNoActionTime();
    };

} // namespace Game
