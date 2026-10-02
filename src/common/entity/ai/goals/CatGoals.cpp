// File: src/common/entity/ai/goals/CatGoals.cpp
#include "common/entity/ai/goals/CatGoals.hpp"

#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"

#include <vector>

namespace Game {

    // ── OcelotAttackGoal ───────────────────────────────────────────────────

    OcelotAttackGoal::OcelotAttackGoal(Mob* mob) : m_mob(mob) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool OcelotAttackGoal::CanUse() {
        LivingEntity* target = m_mob->GetTarget();
        if (!target) return false;
        m_target = target;
        return true;
    }

    bool OcelotAttackGoal::CanContinueToUse() {
        if (!m_target || !m_target->IsAlive()) return false;
        if (m_mob->DistanceToSqr(*m_target) > 225.0) return false;
        return !m_mob->GetNavigation().IsDone() || CanUse();
    }

    void OcelotAttackGoal::Stop() {
        m_target = nullptr;
        m_mob->GetNavigation().Stop();
    }

    void OcelotAttackGoal::Tick() {
        if (!m_target) return;
        m_mob->GetLookControl().SetLookAt(
            m_target->position.x, m_target->GetEyeY(), m_target->position.z,
            30.0f, 30.0f);

        // MC's three speeds: creep at 0.6 while stalking inside 15 blocks,
        // sprint at 1.33 between melee reach and 4 blocks, walk at 0.8
        // otherwise. Cat/Ocelot::CustomServerAiStep reads the chosen speed
        // back off the move control as the crouch pose / sprint flag.
        const double meleeRadiusSqr =
            static_cast<double>(m_mob->GetBbWidth()) * 2.0
            * static_cast<double>(m_mob->GetBbWidth()) * 2.0;
        const double distSqr = m_mob->DistanceToSqr(
            m_target->position.x, m_target->position.y, m_target->position.z);
        double speedModifier = 0.8;
        if (distSqr > meleeRadiusSqr && distSqr < 16.0) {
            speedModifier = 1.33;
        } else if (distSqr < 225.0) {
            speedModifier = 0.6;
        }

        m_mob->GetNavigation().MoveTo(*m_target, speedModifier);
        m_attackTime = std::max(m_attackTime - 1, 0);
        if (distSqr <= meleeRadiusSqr && m_attackTime <= 0) {
            m_attackTime = 20;
            m_mob->DoHurtTarget(*m_target);
        }
    }

    void OcelotAttackGoal::ClearReferenceTo(const Entity* entity) {
        if (m_target == entity) m_target = nullptr;
    }

    // ── Tempt goals ────────────────────────────────────────────────────────

    OcelotTemptGoal::OcelotTemptGoal(Ocelot* ocelot, double speedModifier,
                                     bool canScare)
        : TemptGoal(ocelot, speedModifier, canScare), m_ocelot(ocelot) {}

    bool OcelotTemptGoal::CanScare() const {
        // MC Ocelot.OcelotTemptGoal.canScare: !this.ocelot.isTrusting().
        return !m_ocelot->IsTrusting();
    }

    CatTemptGoal::CatTemptGoal(Cat* cat, double speedModifier, bool canScare)
        : TemptGoal(cat, speedModifier, canScare), m_cat(cat) {}

    bool CatTemptGoal::CanUse() {
        // MC CatTemptGoal.canUse: super && !cat.isTame().
        return TemptGoal::CanUse() && !m_cat->IsTame();
    }

    // ── Avoid goals ────────────────────────────────────────────────────────

    CatAvoidEntityGoal::CatAvoidEntityGoal(Cat* cat, float maxDistance,
                                           double walkSpeedModifier,
                                           double sprintSpeedModifier)
        : AvoidEntityGoal(cat, maxDistance, walkSpeedModifier,
                          sprintSpeedModifier),
          m_cat(cat) {}

    bool CatAvoidEntityGoal::CanUse() {
        return !m_cat->IsTame() && AvoidEntityGoal::CanUse();
    }

    bool CatAvoidEntityGoal::CanContinueToUse() {
        return !m_cat->IsTame() && AvoidEntityGoal::CanContinueToUse();
    }

    OcelotAvoidEntityGoal::OcelotAvoidEntityGoal(Ocelot* ocelot,
                                                 float maxDistance,
                                                 double walkSpeedModifier,
                                                 double sprintSpeedModifier)
        : AvoidEntityGoal(ocelot, maxDistance, walkSpeedModifier,
                          sprintSpeedModifier),
          m_ocelot(ocelot) {}

    bool OcelotAvoidEntityGoal::CanUse() {
        return !m_ocelot->IsTrusting() && AvoidEntityGoal::CanUse();
    }

    bool OcelotAvoidEntityGoal::CanContinueToUse() {
        return !m_ocelot->IsTrusting() && AvoidEntityGoal::CanContinueToUse();
    }

    // ── NonTameRandomTargetGoal ────────────────────────────────────────────

    NonTameRandomTargetGoal::NonTameRandomTargetGoal(Mob* mob,
                                                     EntityTypeId targetType,
                                                     bool mustSee,
                                                     bool babyOnLandOnly)
        : TargetGoal(mob, mustSee, /*mustReach=*/false),
          m_targetTypes{targetType}, m_babyOnLandOnly(babyOnLandOnly) {}

    NonTameRandomTargetGoal::NonTameRandomTargetGoal(Mob* mob,
                                                     const EntityTypeId* targetTypes,
                                                     int targetTypeCount,
                                                     bool mustSee,
                                                     bool babyOnLandOnly)
        : TargetGoal(mob, mustSee, /*mustReach=*/false),
          m_targetTypes(targetTypes, targetTypes + targetTypeCount),
          m_babyOnLandOnly(babyOnLandOnly) {}

    bool NonTameRandomTargetGoal::CanUse() {
        // MC NonTameRandomTargetGoal.canUse: super.canUse() && !isTame() —
        // a tamed hunter stops picking wild prey.
        if (const auto* tamable = dynamic_cast<const TamableAnimal*>(m_mob)) {
            if (tamable->IsTame()) return false;
        }
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        // MC NearestAttackableTargetGoal's default poll: randomInterval 10,
        // stored as reducedTickDelay(10).
        if (level->Random().NextInt(ReducedTickDelay(10)) != 0) return false;

        const double follow = GetFollowDistance();
        // MC NearestAttackableTargetGoal.targetConditions: forCombat over the
        // follow range — line of sight always tested; mustSee only governs
        // TargetGoal.canContinueToUse, which this goal overrides anyway.
        const TargetingConditions conditions =
            TargetingConditions::ForCombat().Range(follow);

        // MC 26.3 getTargetSearchArea: the follow range on ALL axes.
        AABB box = m_mob->GetAABB();
        box.min -= glm::vec3(follow, follow, follow);
        box.max += glm::vec3(follow, follow, follow);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_mob, nearby);

        LivingEntity* best = nullptr;
        double bestDistSq = 0.0;
        for (Entity* e : nearby) {
            bool typeMatch = false;
            for (EntityTypeId t : m_targetTypes) {
                if (e->GetType() == t) { typeMatch = true; break; }
            }
            if (!typeMatch) continue;
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living) continue;
            if (m_babyOnLandOnly && !Turtle::IsBabyOnLand(*living)) continue;
            if (!conditions.Test(m_mob, *living)) continue;
            const double d = m_mob->DistanceToSqr(*living);
            if (!best || d < bestDistSq) { best = living; bestDistSq = d; }
        }

        m_found = best;
        return m_found != nullptr;
    }

    bool NonTameRandomTargetGoal::CanContinueToUse() {
        // MC NonTameRandomTargetGoal.canContinueToUse:
        // targetConditions.test(level, mob, target) — the goal's OWN target
        // (NearestAttackableTargetGoal.target) re-tested with the conditions
        // it was found under, selector included. Unlike TargetGoal's version
        // it never calls setTarget: once Wolf.tryToTame or the owner's sit
        // order clears the mob's target, the hunt does not come back — the
        // goal just idles until its prey leaves the conditions.
        if (!m_found || !m_found->IsAlive()) return false;
        if (m_babyOnLandOnly && !Turtle::IsBabyOnLand(*m_found)) return false;
        const TargetingConditions conditions =
            TargetingConditions::ForCombat().Range(GetFollowDistance());
        return conditions.Test(m_mob, *m_found);
    }

    void NonTameRandomTargetGoal::Start() {
        // MC NearestAttackableTargetGoal.start: setTarget + super only. Not
        // cached into m_targetMob — TargetGoal::CanContinueToUse would then
        // resurrect the prey after taming cleared it (a tamed wolf chasing
        // sheep).
        m_mob->SetTarget(m_found);
        TargetGoal::Start();
    }

    void NonTameRandomTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_found == entity) m_found = nullptr;
    }

} // namespace Game
