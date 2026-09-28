// File: src/common/entity/ai/goals/ParrotGoals.hpp
//
// The parrot's own goals — MC Parrot.ParrotWanderGoal, ai/goal/
// LandOnOwnersShoulderGoal and ai/goal/FollowMobGoal. FollowMobGoal is a
// general MC goal, but the parrot is its only vanilla user.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"

#include <glm/glm.hpp>

namespace Game {

    class Mob;
    class Parrot;
    class PathfinderMob;

    // MC Parrot.ParrotWanderGoal — WaterAvoidingRandomFlyingGoal whose
    // target prefers a perch: an empty cell (with empty headroom) on top of
    // leaves or a log within ±3 horizontally and ±6 vertically; a parrot in
    // water first aims for land.
    class ParrotWanderGoal : public WaterAvoidingRandomFlyingGoal {
    public:
        ParrotWanderGoal(PathfinderMob* mob, double speedModifier)
            : WaterAvoidingRandomFlyingGoal(mob, speedModifier) {}

        const char* Name() const override { return "ParrotWanderGoal"; }

    protected:
        bool GetPosition(glm::dvec3& out) override;

    private:
        // MC ParrotWanderGoal.getTreePos.
        bool GetTreePos(glm::dvec3& out) const;
    };

    // MC ai/goal/LandOnOwnersShoulderGoal — a tame, un-sitting parrot whose
    // ride cooldown has run out lands on its owner's shoulder when their
    // boxes touch (ServerPlayer.setEntityOnShoulder, through the level). No
    // flags: it runs beside the follow/wander goals, and once seated it
    // refuses interruption (isInterruptable) — by then the parrot is gone
    // from the level anyway.
    class LandOnOwnersShoulderGoal : public Goal {
    public:
        explicit LandOnOwnersShoulderGoal(Parrot* entity) : m_entity(entity) {}

        bool CanUse() override;
        bool IsInterruptable() const override { return !m_isSittingOnShoulder; }
        void Start() override { m_isSittingOnShoulder = false; }
        void Tick() override;
        const char* Name() const override { return "LandOnOwnersShoulderGoal"; }

    private:
        Parrot* m_entity;
        bool    m_isSittingOnShoulder = false;
    };

    // MC ai/goal/FollowMobGoal — tag along with the first visible Mob of
    // another kind within `areaSize`: path to it beyond `stopDistance`,
    // otherwise stop, and step away when crowding it or when it is looking
    // straight at this mob. Water cost is zeroed while following.
    class FollowMobGoal : public Goal {
    public:
        FollowMobGoal(Mob* mob, double speedModifier, float stopDistance, float areaSize);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "FollowMobGoal"; }

    private:
        Mob*   m_mob;
        Mob*   m_followingMob = nullptr;
        double m_speedModifier;
        int    m_timeToRecalcPath = 0;
        float  m_stopDistance;
        float  m_oldWaterCost = 0.0f;
        float  m_areaSize;
    };

} // namespace Game
