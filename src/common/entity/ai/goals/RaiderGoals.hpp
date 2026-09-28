// File: src/common/entity/ai/goals/RaiderGoals.hpp
//
// The patrol goals of MC's raider family:
//
//   * PatrollingMonster.LongDistancePatrolGoal — every PatrollingMonster's
//     priority-4 goal (PatrollingMonster.registerGoals): a patrolling mob
//     with a patrol target and no attack target walks toward the target in
//     10-block hops, swung 90 "degrees" (MC passes 90 to Vec3.yRot, which
//     takes RADIANS — the hop really turns by 90 rad) and pulled 40% of the
//     way, so the patrol drifts along an arc. The leader re-rolls its target
//     when within 10 blocks of it and hands each hop to the companions within
//     16 blocks; a patroller with no companion left stops patrolling. A hop
//     the navigator cannot path to becomes a random 8-block step and a
//     200-tick cooldown.
//
//   * Raider.HoldGroundAttackGoal — a patrolling illager outside a raid that
//     has a target but is not yet aggressive (and was not hit by a player)
//     stops, shares its target with every raider within 8 blocks, stares the
//     target down while it is farther than the hostile radius (a 1-in-50
//     ambient call per tick), and turns aggressive once it closes in; when
//     the goal ends with a target, the whole group turns aggressive on it.
#pragma once

#include "common/entity/ai/Goal.hpp"

#include <cstdint>

namespace Game {

    class PatrollingMonster;
    class AbstractIllager;

    class LongDistancePatrolGoal : public Goal {
    public:
        static constexpr int kNavigationFailedCooldown = 200;   // NAVIGATION_FAILED_COOLDOWN

        LongDistancePatrolGoal(PatrollingMonster* mob, double speedModifier, double leaderSpeedModifier);

        bool CanUse() override;
        void Tick() override;
        const char* Name() const override { return "LongDistancePatrolGoal"; }

    private:
        bool MoveRandomly();

        PatrollingMonster* m_mob;
        double  m_speedModifier;
        double  m_leaderSpeedModifier;
        int64_t m_cooldownUntil = -1;
    };

    class HoldGroundAttackGoal : public Goal {
    public:
        HoldGroundAttackGoal(AbstractIllager* mob, float hostileRadius);

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "HoldGroundAttackGoal"; }

    private:
        AbstractIllager* m_mob;
        float m_hostileRadiusSqr;
    };

} // namespace Game
