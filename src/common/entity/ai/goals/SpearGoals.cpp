// File: src/common/entity/ai/goals/SpearGoals.cpp
#include "common/entity/ai/goals/SpearGoals.hpp"

#include "common/core/Mth.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/SpearItem.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace SpearAi {

        bool HoldsKineticWeapon(const Mob& mob) {
            return Spear::Kinetic(mob.GetMainHandEquipment()) != nullptr;
        }

        int KineticUseDuration(const Mob& mob) {
            const Spear::KineticWeapon* kinetic = Spear::Kinetic(mob.GetMainHandEquipment());
            return kinetic ? kinetic->ComputeDamageUseDuration() : 0;
        }

        float ChargeSpeedModifier(Mob& mob) {
            Entity* root = &mob;
            while (root->GetVehicle()) root = root->GetVehicle();
            if (root == &mob) return 1.0f;
            if (auto* vehicle = dynamic_cast<Mob*>(root)) return vehicle->ChargeSpeedModifier();
            return 1.0f;
        }

        void LookAt(Mob& mob, const Entity& target, float yMax, float xMax) {
            const double xd = target.position.x - mob.position.x;
            const double zd = target.position.z - mob.position.z;
            double yd;
            if (const LivingEntity* living = target.AsLiving()) {
                yd = living->GetEyeY() - mob.GetEyeY();
            } else {
                const AABBd box = target.GetAABBd();
                yd = (box.min.y + box.max.y) / 2.0 - mob.GetEyeY();
            }
            const double sd = std::sqrt(xd * xd + zd * zd);
            const float yRotD = static_cast<float>(std::atan2(zd, xd) * 180.0 / 3.14159265358979323846) - 90.0f;
            const float xRotD = static_cast<float>(-(std::atan2(yd, sd) * 180.0 / 3.14159265358979323846));
            // Mob.rotlerp: the wrapped difference, clamped to ±max.
            const auto rotlerp = [](float a, float b, float max) {
                const float diff = std::clamp(Mth::WrapDegrees(b - a), -max, max);
                return a + diff;
            };
            mob.xRot = rotlerp(mob.xRot, xRotD, xMax);
            mob.yRot = rotlerp(mob.yRot, yRotD, yMax);
        }

    } // namespace SpearAi

    namespace {
        // MIN/MAX_REPOSITION_DISTANCE, MIN/MAX_COOLDOWN_DISTANCE and
        // MAX_FLEEING_TIME (reducedTickDelay(100)).
        constexpr int kMinRepositionDistance = 6;
        constexpr int kMaxRepositionDistance = 7;
        constexpr int kMinCooldownDistance = 9;
        constexpr int kMaxCooldownDistance = 11;
    }

    SpearUseGoal::SpearUseGoal(PathfinderMob* mob, double speedModifierWhenCharging,
                               double speedModifierWhenRepositioning, float approachDistance,
                               float targetInRangeRadius)
        : m_mob(mob),
          m_speedModifierWhenCharging(speedModifierWhenCharging),
          m_speedModifierWhenRepositioning(speedModifierWhenRepositioning),
          m_approachDistanceSq(approachDistance * approachDistance),
          m_targetInRangeRadiusSq(targetInRangeRadius * targetInRangeRadius) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool SpearUseGoal::AbleToAttack() const {
        return m_mob->GetTarget() != nullptr && SpearAi::HoldsKineticWeapon(*m_mob);
    }

    bool SpearUseGoal::CanUse() {
        return AbleToAttack() && !m_mob->IsUsingItem();
    }

    bool SpearUseGoal::CanContinueToUse() {
        return m_state && !m_state->done && AbleToAttack();
    }

    void SpearUseGoal::Start() {
        m_mob->SetAggressive(true);
        m_state = State{};
    }

    void SpearUseGoal::Stop() {
        m_mob->GetNavigation().Stop();
        m_mob->SetAggressive(false);
        m_state.reset();
        m_mob->StopUsingItem();
    }

    void SpearUseGoal::Tick() {
        if (!m_state) return;
        LivingEntity* target = m_mob->GetTarget();
        if (!target) return;
        const double targetDistSqr = m_mob->DistanceToSqr(target->position.x, target->position.y,
                                                          target->position.z);
        const float speedModifier = SpearAi::ChargeSpeedModifier(*m_mob);
        const int mountDistance = m_mob->IsPassenger() ? 2 : 0;
        SpearAi::LookAt(*m_mob, *target, 30.0f, 30.0f);
        m_mob->GetLookControl().SetLookAt(target->position.x, target->GetEyeY(), target->position.z,
                                          30.0f, 30.0f);

        if (m_state->NotEngagedYet()) {
            if (targetDistSqr > static_cast<double>(m_approachDistanceSq)) {
                m_mob->GetNavigation().MoveTo(*target, speedModifier * m_speedModifierWhenRepositioning);
                return;
            }
            // The spear goes down: the charge lasts reducedTickDelay of the
            // weapon's damage window.
            m_state->engageTime = ReducedTickDelay(SpearAi::KineticUseDuration(*m_mob));
            m_mob->StartUsingItem(EquipmentSlot::MAINHAND);
        }

        if (m_state->TickAndCheckEngagement()) {
            // The charge is spent: break away, 9-11 blocks off.
            m_mob->StopUsingItem();
            const double distance = std::sqrt(targetDistSqr);
            m_state->awayPos = RandomPos::GetLandPosAway(
                *m_mob, std::max(0.0, static_cast<double>(kMinCooldownDistance + mountDistance) - distance),
                std::max(1.0, static_cast<double>(kMaxCooldownDistance + mountDistance) - distance), 7,
                target->position);
            m_state->fleeingTime = 1;
        }

        if (m_state->TickAndCheckFleeing(static_cast<double>(ReducedTickDelay(100)))) return;
        if (m_state->awayPos) {
            const glm::dvec3 away = *m_state->awayPos;
            m_mob->GetNavigation().MoveTo(away.x, away.y, away.z,
                                          speedModifier * m_speedModifierWhenRepositioning);
            if (m_mob->GetNavigation().IsDone()) {
                if (m_state->fleeingTime > 0) {
                    m_state->done = true;
                    return;
                }
                m_state->awayPos.reset();
            }
        } else {
            // Charge: at the target, and past it once it is in reach.
            m_mob->GetNavigation().MoveTo(*target, speedModifier * m_speedModifierWhenCharging);
            if (targetDistSqr < static_cast<double>(m_targetInRangeRadiusSq) || m_mob->GetNavigation().IsDone()) {
                const double distance = std::sqrt(targetDistSqr);
                m_state->awayPos = RandomPos::GetLandPosAway(
                    *m_mob, static_cast<double>(kMinRepositionDistance + mountDistance) - distance,
                    static_cast<double>(kMaxRepositionDistance + mountDistance) - distance, 7,
                    target->position);
            }
        }
    }

} // namespace Game
