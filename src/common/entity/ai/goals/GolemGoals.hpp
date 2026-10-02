// File: src/common/entity/ai/goals/GolemGoals.hpp
//
// The iron golem's village goals — MC world/entity/ai/goal
// MoveTowardsTargetGoal, MoveBackToVillageGoal,
// GolemRandomStrollInVillageGoal, OfferFlowerGoal and
// target/DefendVillageTargetGoal — on the village layer (PoiManager's
// sectionsToVillage / getInRange, the villagers' gossip reputation and
// wantsToSpawnGolem). Only IronGolem registers them in vanilla.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/physics/Physics.hpp"

#include <glm/glm.hpp>

#include <optional>

namespace Game {

    class IronGolem;
    class LivingEntity;
    class PathfinderMob;

    // MC MoveTowardsTargetGoal: a target within `within` blocks draws the
    // mob to a DefaultRandomPos rolled toward it (16 x 7, half-pi cone) —
    // the golem closing in before MeleeAttackGoal's reach takes over.
    class MoveTowardsTargetGoal : public Goal {
    public:
        MoveTowardsTargetGoal(PathfinderMob* mob, double speedModifier, float within);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "MoveTowardsTargetGoal"; }

    private:
        PathfinderMob* m_mob;
        LivingEntity*  m_target = nullptr;
        double m_wantedX = 0.0, m_wantedY = 0.0, m_wantedZ = 0.0;
        double m_speedModifier;
        float  m_within;
    };

    // MC MoveBackToVillageGoal (a RandomStrollGoal, interval 10): outside a
    // village (isVillage), stroll toward the closest village section within
    // two sections (BehaviorUtils.findSectionClosestToVillage).
    class MoveBackToVillageGoal : public RandomStrollGoal {
    public:
        MoveBackToVillageGoal(PathfinderMob* mob, double speedModifier, bool checkNoActionTime);

        bool CanUse() override;
        const char* Name() const override { return "MoveBackToVillageGoal"; }

    protected:
        bool GetPosition(glm::dvec3& out) override;
    };

    // MC GolemRandomStrollInVillageGoal (a RandomStrollGoal, interval 240,
    // no noActionTime check): 30% anywhere (LandRandomPos), else toward a
    // villager that wants a golem or an occupied POI of a village section
    // around it (70 / 30 which is tried first), anywhere as the fallback.
    class GolemRandomStrollInVillageGoal : public RandomStrollGoal {
    public:
        GolemRandomStrollInVillageGoal(PathfinderMob* mob, double speedModifier);

        const char* Name() const override { return "GolemRandomStrollInVillageGoal"; }

    protected:
        bool GetPosition(glm::dvec3& out) override;

    private:
        std::optional<glm::dvec3> GetPositionTowardsAnywhere();
        std::optional<glm::dvec3> GetPositionTowardsVillagerWhoWantsGolem();
        std::optional<glm::dvec3> GetPositionTowardsPoi();
    };

    // MC OfferFlowerGoal: by day, 1 in 8000, the golem holds a poppy out to
    // the nearest #candidate_for_iron_golem_gift within 6 (villagers, copper
    // golems) for 400 ticks, looking at it; a copper golem still beside it
    // at the end (#accepts_iron_golem_gift, empty antenna) takes the poppy
    // onto its antenna as a guaranteed drop.
    class OfferFlowerGoal : public Goal {
    public:
        static constexpr int kOfferTicks = 400;   // OFFER_TICKS

        explicit OfferFlowerGoal(IronGolem* golem);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "OfferFlowerGoal"; }

    private:
        // getGolemBoundingBox: the golem's box inflated (6, 2, 6).
        AABBd GolemBoundingBox() const;

        IronGolem*          m_golem;
        LivingEntity*       m_entity = nullptr;
        int                 m_tick = 0;
        TargetingConditions m_offerTargetContext;   // OFFER_TARGET_CONTEXT
    };

    // MC DefendVillageTargetGoal (mustSee false, mustReach true): a player
    // whose reputation with any villager in the golem's (10, 8, 10) box is
    // -100 or worse becomes its target — unless spectating or creative. As
    // in MC the candidate is never cleared between evaluations.
    class DefendVillageTargetGoal : public TargetGoal {
    public:
        explicit DefendVillageTargetGoal(IronGolem* golem);

        bool CanUse() override;
        void Start() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "DefendVillageTargetGoal"; }

    private:
        IronGolem*          m_golem;
        LivingEntity*       m_potentialTarget = nullptr;
        TargetingConditions m_attackTargeting;   // forCombat().range(64)
    };

} // namespace Game
