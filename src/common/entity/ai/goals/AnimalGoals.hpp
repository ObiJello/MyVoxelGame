// File: src/common/entity/ai/goals/AnimalGoals.hpp
//
// The passive-mob goals: being tempted by food, breeding, following a parent,
// and (for sheep) eating grass.
//
// TemptGoal's "canScare" rule is the subtle one. A tempted animal follows you
// while you hold food, but if you MOVE or TURN too much while it is within 6
// blocks it gets spooked and stops for 50 evaluations. That is what makes
// leading animals feel like coaxing rather than dragging, and it is why the
// goal caches your position and rotation on start.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/TargetingConditions.hpp"

#include <cstdint>

namespace Game {

    class Animal;
    class PathfinderMob;
    class LivingEntity;

    class TemptGoal : public Goal {
    public:
        static constexpr double kDefaultStopDistance = 2.5;

        TemptGoal(PathfinderMob* mob, double speedModifier, bool canScare);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "TemptGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

        bool IsRunning() const { return m_isRunning; }

    private:
        PathfinderMob* m_mob;
        LivingEntity*  m_player = nullptr;
        double m_speedModifier;
        bool   m_canScare;
        bool   m_isRunning = false;
        int    m_calmDown = 0;
        double m_px = 0.0, m_py = 0.0, m_pz = 0.0;
        float  m_pRotX = 0.0f, m_pRotY = 0.0f;
        TargetingConditions m_conditions;
    };

    class BreedGoal : public Goal {
    public:
        BreedGoal(Animal* animal, double speedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "BreedGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Animal* GetFreePartner();

        Animal* m_animal;
        Animal* m_partner = nullptr;
        double  m_speedModifier;
        int     m_loveTime = 0;
    };

    // MC FollowParentGoal. Registers NO flags on purpose — a following baby can
    // still look around and panic, and MC relies on that.
    class FollowParentGoal : public Goal {
    public:
        static constexpr int kHorizontalScanRange = 8;
        static constexpr int kVerticalScanRange = 4;
        static constexpr int kDontFollowIfCloserThan = 3;

        FollowParentGoal(Animal* animal, double speedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "FollowParentGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Animal* m_animal;
        Animal* m_parent = nullptr;
        double  m_speedModifier;
        int     m_timeToRecalcPath = 0;
    };

    // MC EatBlockGoal — the sheep grazing animation, which also regrows wool.
    class EatBlockGoal : public Goal {
    public:
        static constexpr int kEatAnimationTicks = 40;

        explicit EatBlockGoal(Animal* animal);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "EatBlockGoal"; }

        // NOTE: RequiresUpdateEveryTick is deliberately NOT overridden — MC's
        // EatBlockGoal does not override it either, so it ticks only on the
        // selector's even ticks and every duration inside goes through
        // AdjustedTickDelay to compensate. It used to return true here, which
        // silently un-halved all three numbers and made sheep graze at half
        // the vanilla rate.

        int GetEatAnimationTick() const { return m_eatAnimationTick; }

        // DEBUG ONLY (/sheepeat): let the next CanUse skip its random roll.
        // Everything else still applies — there must still be an edible block —
        // so the forced path exercises the real one rather than faking it.
        void ForceNextUse() { m_forceNextUse = true; }

    private:
        Animal* m_animal;
        int     m_eatAnimationTick = 0;
        bool    m_forceNextUse = false;
    };

} // namespace Game
