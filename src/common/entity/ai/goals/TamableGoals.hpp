// File: src/common/entity/ai/goals/TamableGoals.hpp
//
// The owner-gated goals every TamableAnimal shares — MC's
// SitWhenOrderedToGoal, FollowOwnerGoal, TamableAnimal.TamableAnimalPanicGoal,
// and the target pair OwnerHurtByTargetGoal / OwnerHurtTargetGoal. They live
// in one file because in MC they are all typed on TamableAnimal and shared by
// wolf, cat and parrot alike.
//
// Every constructor takes BOTH halves of the implementer (the Mob* and the
// TamableAnimal* mixin) — the same object, passed twice, because the mixin is
// not on the Mob inheritance chain (see TamableAnimal.hpp's header note).
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/world/pathfinder/PathType.hpp"

namespace Game {

    class Mob;
    class LivingEntity;
    class TamableAnimal;
    class Wolf;

    // MC ai/goal/BegGoal — a nearby player holding a bone or wolf food gets
    // stared at with the interested head-tilt for 40+rand(40) ticks. LOOK
    // flag only; the interested flag rides the wolf's anim byte (bit 2) and
    // the renderer turns it into the model's headRollAngle.
    class BegGoal : public Goal {
    public:
        BegGoal(Wolf* wolf, float lookDistance);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "BegGoal"; }

    private:
        // MC BegGoal.playerHoldingInteresting: a bone, or anything the wolf
        // eats. One hand only — the wire carries no off-hand.
        bool PlayerHoldingInteresting() const;

        Wolf*         m_wolf;
        LivingEntity* m_player = nullptr;
        float         m_lookDistance;
        int           m_lookTime = 0;
    };

    // MC ai/goal/SitWhenOrderedToGoal — hold the sitting pose while ordered.
    // The refusals are MC's: not in water, not mid-air, and not while the
    // nearby owner is being attacked.
    class SitWhenOrderedToGoal : public Goal {
    public:
        SitWhenOrderedToGoal(Mob* mob, TamableAnimal* tamable);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "SitWhenOrderedToGoal"; }

    private:
        Mob*           m_mob;
        TamableAnimal* m_tamable;
    };

    // MC ai/goal/FollowOwnerGoal — path to the owner beyond startDistance,
    // stop inside stopDistance, teleport into the ±3 ring past 12 blocks
    // (TamableAnimal.shouldTryTeleportToOwner). Water cost is zeroed while
    // following, exactly as MC does, so the follower swims after you.
    class FollowOwnerGoal : public Goal {
    public:
        FollowOwnerGoal(Mob* mob, TamableAnimal* tamable, double speedModifier,
                        float startDistance, float stopDistance);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "FollowOwnerGoal"; }

    private:
        Mob*           m_mob;
        TamableAnimal* m_tamable;
        LivingEntity*  m_owner = nullptr;
        double         m_speedModifier;
        float          m_startDistance;
        float          m_stopDistance;
        int            m_timeToRecalcPath = 0;
        float          m_oldWaterCost = 0.0f;
    };

    // MC TamableAnimal.TamableAnimalPanicGoal — PanicGoal whose tick also
    // teleports to the owner when the flight has carried the mob out of
    // range, so a panicked pet does not strand itself.
    class TamableAnimalPanicGoal : public PanicGoal {
    public:
        TamableAnimalPanicGoal(PathfinderMob* mob, TamableAnimal* tamable,
                               double speedModifier);

        void Tick() override;
        const char* Name() const override { return "TamableAnimalPanicGoal"; }

    private:
        TamableAnimal* m_tamable;
    };

    // MC target/OwnerHurtByTargetGoal — attack whatever last hurt the owner.
    class OwnerHurtByTargetGoal : public TargetGoal {
    public:
        OwnerHurtByTargetGoal(Mob* mob, TamableAnimal* tamable);

        bool CanUse() override;
        void Start() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "OwnerHurtByTargetGoal"; }

    private:
        TamableAnimal* m_tamable;
        LivingEntity*  m_ownerLastHurtBy = nullptr;
        int64_t        m_timestamp = 0;
    };

    // MC target/OwnerHurtTargetGoal — attack whatever the owner last hurt.
    class OwnerHurtTargetGoal : public TargetGoal {
    public:
        OwnerHurtTargetGoal(Mob* mob, TamableAnimal* tamable);

        bool CanUse() override;
        void Start() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "OwnerHurtTargetGoal"; }

    private:
        TamableAnimal* m_tamable;
        LivingEntity*  m_ownerLastHurt = nullptr;
        int64_t        m_timestamp = 0;
    };

} // namespace Game
