// File: src/common/entity/ai/goals/TargetGoals.cpp
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/core/JavaRandom.hpp"

#include <vector>

namespace Game {

    // ── TargetGoal ─────────────────────────────────────────────────────────

    TargetGoal::TargetGoal(Mob* mob, bool mustSee, bool mustReach)
        : m_mob(mob), m_mustSee(mustSee), m_mustReach(mustReach) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Target));
    }

    double TargetGoal::GetFollowDistance() const {
        return m_mob->GetAttributeValue(Attribute::FollowRange);
    }

    bool TargetGoal::CanContinueToUse() {
        LivingEntity* target = m_mob->GetTarget();
        if (!target) target = m_targetMob;
        if (!target) return false;

        if (!target->IsAlive()) return false;
        if (!m_mob->CanAttack(*target)) return false;

        const double follow = GetFollowDistance();
        if (m_mob->DistanceToSqr(*target) > follow * follow) return false;

        if (m_mustSee) {
            if (m_mob->GetSensing().HasLineOfSight(*target)) {
                m_unseenTicks = 0;
            } else if (++m_unseenTicks > ReducedTickDelay(m_unseenMemoryTicks)) {
                // Memory expired. This is the only thing that lets a player
                // break pursuit by hiding rather than by outrunning.
                return false;
            }
        }

        m_mob->SetTarget(target);
        return true;
    }

    void TargetGoal::Start() {
        m_unseenTicks = 0;
    }

    void TargetGoal::Stop() {
        m_mob->SetTarget(nullptr);
        m_targetMob = nullptr;
    }

    // ── HurtByTargetGoal ───────────────────────────────────────────────────

    HurtByTargetGoal::HurtByTargetGoal(Mob* mob)
        : TargetGoal(mob, true, false) {}

    bool HurtByTargetGoal::CanUse() {
        // The timestamp comparison is what makes this fire ONCE per hit rather
        // than continuously while the memory lasts.
        const int64_t stamp = m_mob->GetLastHurtByMobTimestamp();
        Entity* attacker = m_mob->GetLastHurtByMob();
        if (!attacker || stamp == m_timestamp) return false;

        LivingEntity* living = dynamic_cast<LivingEntity*>(attacker);
        if (!living || !living->IsAlive()) return false;

        // MC uses HURT_BY_TARGETING: ignores line of sight AND invisibility.
        return m_mob->CanAttack(*living);
    }

    void HurtByTargetGoal::Start() {
        Entity* attacker = m_mob->GetLastHurtByMob();
        if (LivingEntity* living = dynamic_cast<LivingEntity*>(attacker)) {
            m_mob->SetTarget(living);
            m_targetMob = living;
        }
        m_timestamp = m_mob->GetLastHurtByMobTimestamp();
        // Retaliation memory is five times longer than ordinary pursuit.
        m_unseenMemoryTicks = 300;

        if (m_alertOthers) AlertOthers();
        TargetGoal::Start();
    }

    void HurtByTargetGoal::AlertOthers() {
        EntityLevel* level = m_mob->Level();
        if (!level) return;

        LivingEntity* attacker = m_mob->GetTarget();
        if (!attacker) return;

        // MC's alert box: follow range horizontally, 10 blocks vertically.
        const double follow = GetFollowDistance();
        AABB box = m_mob->GetAABB();
        box.min -= glm::vec3(follow, 10.0, follow);
        box.max += glm::vec3(follow, 10.0, follow);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_mob, nearby);

        for (Entity* e : nearby) {
            Mob* other = dynamic_cast<Mob*>(e);
            // Same species only, and only ones not already busy — MC will not
            // steal a target a mob has already chosen.
            if (!other || other->GetType() != m_mob->GetType()) continue;
            if (other->GetTarget()) continue;
            other->SetTarget(attacker);
        }
    }

    // ── NearestAttackableTargetGoal ────────────────────────────────────────

    NearestAttackableTargetGoal::NearestAttackableTargetGoal(Mob* mob, bool mustSee,
                                                             bool mustReach, int randomInterval)
        : TargetGoal(mob, mustSee, mustReach),
          m_randomInterval(ReducedTickDelay(randomInterval)) {
        m_conditions = TargetingConditions::ForCombat();
    }

    NearestAttackableTargetGoal::NearestAttackableTargetGoal(
            Mob* mob, const EntityTypeId* types, int typeCount,
            bool mustSee, bool mustReach, int randomInterval)
        : TargetGoal(mob, mustSee, mustReach),
          m_types(types), m_typeCount(typeCount), m_targetsPlayers(false),
          m_randomInterval(ReducedTickDelay(randomInterval)) {
        m_conditions = TargetingConditions::ForCombat();
    }

    void NearestAttackableTargetGoal::FindTarget() {
        EntityLevel* level = m_mob->Level();
        if (!level) { m_target = nullptr; return; }

        const double follow = GetFollowDistance();
        m_conditions.range = follow;

        if (m_targetsPlayers) {
            LivingEntity* nearest = level->GetNearestPlayer(
                m_mob->position.x, m_mob->GetEyeY(), m_mob->position.z, follow);
            m_target = (nearest && m_conditions.Test(m_mob, *nearest)) ? nearest : nullptr;
            return;
        }

        // MC getTargetSearchArea: the follow range horizontally, but only 4
        // blocks vertically. A zombie does not notice a villager two floors up.
        AABB box = m_mob->GetAABB();
        box.min -= glm::vec3(follow, 4.0, follow);
        box.max += glm::vec3(follow, 4.0, follow);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_mob, nearby);

        LivingEntity* best = nullptr;
        double bestDistSq = 0.0;
        for (Entity* e : nearby) {
            bool wanted = false;
            for (int i = 0; i < m_typeCount; ++i) {
                if (e->GetType() == m_types[i]) { wanted = true; break; }
            }
            if (!wanted) continue;

            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !m_conditions.Test(m_mob, *living)) continue;

            const double d = m_mob->DistanceToSqr(*living);
            if (!best || d < bestDistSq) { best = living; bestDistSq = d; }
        }
        m_target = best;
    }

    bool NearestAttackableTargetGoal::CanUse() {
        // Only ~1 evaluation in 5 actually searches. MC does this purely for
        // cost: target acquisition is the most expensive thing a crowd of
        // monsters does, and a few ticks of latency is invisible.
        if (m_randomInterval > 0 &&
            m_mob->Level()->Random().NextInt(m_randomInterval) != 0) {
            return false;
        }

        if (m_extraCondition && !m_extraCondition(*m_mob)) return false;

        FindTarget();
        return m_target != nullptr;
    }

    void NearestAttackableTargetGoal::Start() {
        m_mob->SetTarget(m_target);
        m_targetMob = m_target;
        TargetGoal::Start();
    }

    void TargetGoal::ClearReferenceTo(const Entity* entity) {
        if (m_targetMob == entity) m_targetMob = nullptr;
    }

    void NearestAttackableTargetGoal::ClearReferenceTo(const Entity* entity) {
        if (m_target == entity) m_target = nullptr;
    }

} // namespace Game
