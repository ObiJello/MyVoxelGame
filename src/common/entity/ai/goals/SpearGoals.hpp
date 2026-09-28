// File: src/common/entity/ai/goals/SpearGoals.hpp
//
// MC net.minecraft.world.entity.ai.goal.SpearUseGoal — how a zombie (and a
// husk, a zombified piglin, a zombie horseman) fights with a spear: close to
// the approach distance, lower the spear (startUsingItem — the charge,
// KineticWeapon.damageEntities every tick of the hold, SpearItem.cpp) and run
// at the target, veer off 6-7 blocks past it when it is within reach (or the
// path ends), and when the charge runs out (computeDamageUseDuration) retreat
// 9-11 blocks away before the next round. A mounted user runs everything at
// its mount's chargeSpeedModifier and keeps 2 blocks more distance.
//
// The piglin's brain has the same fight as three behaviours
// (SpearApproach / SpearAttack / SpearRetreat — PiglinAi.cpp); the shared
// helpers are here.
#pragma once

#include "common/entity/ai/Goal.hpp"

#include <glm/glm.hpp>

#include <optional>

namespace Game {

    class Entity;
    class Mob;
    class PathfinderMob;

    namespace SpearAi {
        // mob.getMainHandItem().has(KINETIC_WEAPON).
        bool HoldsKineticWeapon(const Mob& mob);
        // KineticWeapon.computeDamageUseDuration of the main-hand spear; 0
        // without one.
        int  KineticUseDuration(const Mob& mob);
        // The root vehicle's Mob.chargeSpeedModifier (1 on foot).
        float ChargeSpeedModifier(Mob& mob);
        // MC Mob.lookAt(entity, yMax, xMax): the body's yRot/xRot turned
        // toward the target's eyes, at most yMax / xMax degrees a call.
        void LookAt(Mob& mob, const Entity& target, float yMax, float xMax);
    } // namespace SpearAi

    class SpearUseGoal : public Goal {
    public:
        SpearUseGoal(PathfinderMob* mob, double speedModifierWhenCharging,
                     double speedModifierWhenRepositioning, float approachDistance,
                     float targetInRangeRadius);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "SpearUseGoal"; }

    private:
        // MC SpearUseGoal.SpearUseState.
        struct State {
            int  engageTime = -1;
            int  fleeingTime = -1;
            std::optional<glm::dvec3> awayPos;
            bool done = false;

            bool NotEngagedYet() const { return engageTime < 0; }
            bool TickAndCheckEngagement() {
                if (engageTime > 0 && --engageTime == 0) return true;
                return false;
            }
            bool TickAndCheckFleeing(double maxFleeingTime) {
                if (fleeingTime > 0) {
                    ++fleeingTime;
                    if (static_cast<double>(fleeingTime) > maxFleeingTime) {
                        done = true;
                        return true;
                    }
                }
                return false;
            }
        };

        bool AbleToAttack() const;

        PathfinderMob* m_mob;
        std::optional<State> m_state;
        double m_speedModifierWhenCharging;
        double m_speedModifierWhenRepositioning;
        float  m_approachDistanceSq;
        float  m_targetInRangeRadiusSq;
    };

} // namespace Game
