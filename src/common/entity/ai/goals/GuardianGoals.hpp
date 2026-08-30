// File: src/common/entity/ai/goals/GuardianGoals.hpp
//
// MC Guardian.java's nested AI: GuardianAttackGoal (the beam charge) and the
// GuardianAttackSelector-driven target goal. MC nests both inside
// Guardian.java; they live here because every goal class in this port does.
//
// The beam VISUAL is the one skipped piece (no beam render pipeline —
// DATA_ID_ATTACK_TARGET's entity id and the renderer's coloured ray); the
// charge timing, target invalidation, damage math and move-control interplay
// are all ported exactly.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"

namespace Game {

    class Guardian;

    // MC Guardian.GuardianAttackGoal — hold still, stare, and after
    // getAttackDuration() ticks deal the beam damage in one burst.
    class GuardianAttackGoal : public Goal {
    public:
        explicit GuardianAttackGoal(Guardian* guardian);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "GuardianAttackGoal"; }

    private:
        Guardian* m_guardian;
        int  m_attackTime = 0;
        // MC: `guardian instanceof ElderGuardian`, latched at construction.
        bool m_elder;
    };

    // MC Guardian.registerGoals target 1: NearestAttackableTargetGoal(
    // LivingEntity.class, 10, true, false, GuardianAttackSelector) — the
    // selector accepts players, squid and axolotls, and only beyond 3 blocks
    // (distSqr > 9): a guardian never beams something already on top of it.
    class GuardianAttackTargetGoal : public TargetGoal {
    public:
        explicit GuardianAttackTargetGoal(Guardian* guardian);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "GuardianAttackTargetGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        void FindTarget();

        Guardian*           m_guardian;
        LivingEntity*       m_target = nullptr;
        int                 m_randomInterval;
        TargetingConditions m_conditions;
    };

} // namespace Game
