// File: src/common/entity/ai/goals/WitherGoals.hpp
//
// MC Wither.java's nested goal plus its LIVING_ENTITY_SELECTOR target
// goal. MC nests the do-nothing goal inside Wither.java and builds the
// target goal inline from NearestAttackableTargetGoal(LivingEntity.class, 0,
// false, false, LIVING_ENTITY_SELECTOR); both live here because every goal
// class in this port does.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"

namespace Game {

    class Wither;

    // MC Wither.WitherDoNothingGoal — claims MOVE|JUMP|LOOK for the whole
    // 220-tick spawn phase, so the wither hangs motionless while charging up.
    class WitherDoNothingGoal : public Goal {
    public:
        explicit WitherDoNothingGoal(Wither* wither);

        bool CanUse() override;
        const char* Name() const override { return "WitherDoNothingGoal"; }

    private:
        Wither* m_wither;
    };

    // MC's target 2: any attackable living entity that is not a WITHER_FRIEND
    // (the #undead tag), randomInterval 0 (a search on every evaluation),
    // mustSee false, mustReach false.
    class WitherTargetGoal : public TargetGoal {
    public:
        explicit WitherTargetGoal(Wither* wither);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "WitherTargetGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        void FindTarget();

        Wither*         m_wither;
        LivingEntity*       m_target = nullptr;
        TargetingConditions m_conditions;
    };

} // namespace Game
