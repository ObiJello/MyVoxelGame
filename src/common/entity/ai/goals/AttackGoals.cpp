// File: src/common/entity/ai/goals/AttackGoals.cpp
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Game {

    // ── MeleeAttackGoal ────────────────────────────────────────────────────

    MeleeAttackGoal::MeleeAttackGoal(PathfinderMob* mob, double speedModifier,
                                     bool followingTargetEvenIfNotSeen)
        : m_mob(mob), m_speedModifier(speedModifier),
          m_followingTargetEvenIfNotSeen(followingTargetEvenIfNotSeen) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool MeleeAttackGoal::CanUse() {
        // Hard throttle: at most one A* per second while looking for a way in.
        // CanUse runs every other tick, and creating a path is by far the most
        // expensive thing a monster does.
        const int64_t now = m_mob->Level()->GetGameTime();
        if (now - m_lastCanUseCheck < kCooldownBetweenCanUseChecks) return false;
        m_lastCanUseCheck = now;

        LivingEntity* target = m_mob->GetTarget();
        if (!target || !target->IsAlive()) return false;

        m_path = m_mob->GetNavigation().CreatePath(*target, 0);
        if (m_path) return true;

        // No path, but already touching the target — a mob wedged against you
        // in a doorway still gets to swing.
        return m_mob->IsWithinMeleeAttackRange(*target);
    }

    bool MeleeAttackGoal::CanContinueToUse() {
        LivingEntity* target = m_mob->GetTarget();
        if (!target || !target->IsAlive()) return false;

        if (!m_followingTargetEvenIfNotSeen) {
            return !m_mob->GetNavigation().IsDone();
        }

        if (target->IsCreative() || target->IsSpectator()) return false;
        return true;
    }

    void MeleeAttackGoal::Start() {
        m_mob->GetNavigation().MoveTo(m_path, m_speedModifier);
        m_mob->SetAggressive(true);
        m_ticksUntilNextPathRecalculation = 0;
        m_ticksUntilNextAttack = 0;
    }

    void MeleeAttackGoal::Stop() {
        LivingEntity* target = m_mob->GetTarget();
        if (target && (target->IsCreative() || target->IsSpectator())) {
            m_mob->SetTarget(nullptr);
        }
        m_mob->SetAggressive(false);
        m_mob->GetNavigation().Stop();
    }

    bool MeleeAttackGoal::CanPerformAttack(LivingEntity& target) {
        return IsTimeToAttack() && m_mob->IsWithinMeleeAttackRange(target) &&
               m_mob->GetSensing().HasLineOfSight(target);
    }

    void MeleeAttackGoal::CheckAndPerformAttack(LivingEntity& target) {
        if (!CanPerformAttack(target)) return;
        ResetAttackCooldown();
        m_mob->Swing();
        m_mob->DoHurtTarget(target);
    }

    void MeleeAttackGoal::Tick() {
        LivingEntity* target = m_mob->GetTarget();
        if (!target) return;

        m_mob->GetLookControl().SetLookAt(target->position.x, target->GetEyeY(),
                                          target->position.z, 30.0f, 30.0f);

        m_ticksUntilNextPathRecalculation = std::max(m_ticksUntilNextPathRecalculation - 1, 0);

        const bool canSee = m_followingTargetEvenIfNotSeen ||
                            m_mob->GetSensing().HasLineOfSight(*target);

        // Re-path when the target has actually moved a block from where we last
        // pathed to — plus a 5% random chance so a stationary target still gets
        // an occasional refresh (which is how a mob recovers when the terrain
        // between you changed).
        const double movedSq = target->DistanceToSqr(m_pathedTargetX, m_pathedTargetY, m_pathedTargetZ);
        const bool neverPathed = m_pathedTargetX == 0.0 && m_pathedTargetY == 0.0 && m_pathedTargetZ == 0.0;
        const bool shouldRepath = canSee && m_ticksUntilNextPathRecalculation <= 0 &&
                                  (neverPathed || movedSq >= 1.0 ||
                                   m_mob->Level()->Random().NextFloat() < 0.05f);

        if (shouldRepath) {
            m_pathedTargetX = target->position.x;
            m_pathedTargetY = target->position.y;
            m_pathedTargetZ = target->position.z;

            m_ticksUntilNextPathRecalculation = 4 + m_mob->Level()->Random().NextInt(7);

            // Distant targets get re-pathed less often — the path is stale by
            // the same fraction either way, but the search costs the same.
            const double targetDistanceSqr = m_mob->DistanceToSqr(*target);
            if (targetDistanceSqr > 1024.0)     m_ticksUntilNextPathRecalculation += 10;
            else if (targetDistanceSqr > 256.0) m_ticksUntilNextPathRecalculation += 5;

            // A FAILED path is penalised hardest. Without this a mob walled off
            // from its target re-runs A* forever at the base cadence.
            if (!m_mob->GetNavigation().MoveTo(*target, m_speedModifier)) {
                m_ticksUntilNextPathRecalculation += 15;
            }

            m_ticksUntilNextPathRecalculation = AdjustedTickDelay(m_ticksUntilNextPathRecalculation);
        }

        m_ticksUntilNextAttack = std::max(m_ticksUntilNextAttack - 1, 0);
        CheckAndPerformAttack(*target);
    }

    // ── ZombieAttackGoal ───────────────────────────────────────────────────

    ZombieAttackGoal::ZombieAttackGoal(PathfinderMob* mob, double speedModifier,
                                       bool followingTargetEvenIfNotSeen)
        : MeleeAttackGoal(mob, speedModifier, followingTargetEvenIfNotSeen) {}

    void ZombieAttackGoal::Start() {
        MeleeAttackGoal::Start();
        m_raiseArmTicks = 0;
    }

    void ZombieAttackGoal::Stop() {
        MeleeAttackGoal::Stop();
        m_mob->SetAggressive(false);
    }

    void ZombieAttackGoal::Tick() {
        MeleeAttackGoal::Tick();
        ++m_raiseArmTicks;
        // MC raises the arms after 5 ticks and drops them once the zombie is
        // close enough to swing. The renderer reads IsAggressive for the pose.
        if (m_raiseArmTicks >= 5 && m_ticksUntilNextAttack < AdjustedTickDelay(kAttackInterval) / 2) {
            m_mob->SetAggressive(true);
        } else {
            m_mob->SetAggressive(false);
        }
    }

    // ── SwellGoal ──────────────────────────────────────────────────────────

    SwellGoal::SwellGoal(Creeper* creeper) : m_creeper(creeper) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool SwellGoal::CanUse() {
        LivingEntity* target = m_creeper->GetTarget();
        // Already lit, OR a target inside 3 blocks. The `swellDir > 0` half is
        // what makes a primed creeper keep swelling even if you back off — it
        // is Tick() that decides to stand down, not CanUse().
        return m_creeper->GetSwellDir() > 0 ||
               (target && target->IsAlive() && m_creeper->DistanceToSqr(*target) < 9.0);
    }

    void SwellGoal::Start() {
        m_creeper->GetNavigation().Stop();
        m_target = m_creeper->GetTarget();
    }

    void SwellGoal::Tick() {
        if (!m_target || !m_target->IsAlive()) {
            m_creeper->SetSwellDir(-1);
        } else if (m_creeper->DistanceToSqr(*m_target) > 49.0) {
            m_creeper->SetSwellDir(-1);
        } else if (!m_creeper->GetSensing().HasLineOfSight(*m_target)) {
            m_creeper->SetSwellDir(-1);
        } else {
            m_creeper->SetSwellDir(1);
        }
    }

    // ── LeapAtTargetGoal ───────────────────────────────────────────────────

    LeapAtTargetGoal::LeapAtTargetGoal(Mob* mob, float yd) : m_mob(mob), m_yd(yd) {
        SetFlags(GoalFlag::Jump | GoalFlag::Move);
    }

    bool LeapAtTargetGoal::CanUse() {
        m_target = m_mob->GetTarget();
        if (!m_target) return false;

        const double d2 = m_mob->DistanceToSqr(*m_target);
        // The 4..16 window is deliberate: too close and the leap overshoots,
        // too far and it lands short. Outside it the mob just walks.
        if (d2 < 4.0 || d2 > 16.0) return false;
        if (!m_mob->onGround) return false;

        return m_mob->Level()->Random().NextInt(ReducedTickDelay(5)) == 0;
    }

    bool LeapAtTargetGoal::CanContinueToUse() {
        return !m_mob->onGround;
    }

    void LeapAtTargetGoal::Start() {
        glm::dvec3 delta = m_target->position - m_mob->position;
        delta.y = 0.0;

        const double len = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        if (len < 1.0e-7) return;

        // MC blends the leap direction with existing motion 2:1, so a spider
        // already moving does not stop dead to jump.
        const glm::dvec3 dir = delta / len;
        m_mob->velocity.x = m_mob->velocity.x * 0.2 + dir.x * 0.8;
        m_mob->velocity.z = m_mob->velocity.z * 0.2 + dir.z * 0.8;
        m_mob->velocity.y = m_yd;
        m_mob->needsSync = true;
    }

    // ── AvoidPlayerGoal ────────────────────────────────────────────────────

    AvoidEntityGoal::AvoidEntityGoal(PathfinderMob* mob, float maxDistance,
                                     double walkSpeedModifier, double sprintSpeedModifier)
        : m_mob(mob), m_maxDistance(maxDistance),
          m_walkSpeedModifier(walkSpeedModifier), m_sprintSpeedModifier(sprintSpeedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    AvoidEntityGoal::AvoidEntityGoal(PathfinderMob* mob, const EntityTypeId* types,
                                     int typeCount, float maxDistance,
                                     double walkSpeedModifier, double sprintSpeedModifier)
        : m_mob(mob), m_types(types), m_typeCount(typeCount), m_avoidsPlayers(false),
          m_maxDistance(maxDistance),
          m_walkSpeedModifier(walkSpeedModifier), m_sprintSpeedModifier(sprintSpeedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool AvoidEntityGoal::CanUse() {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;

        m_toAvoid = FindThreat(*level);
        if (!m_toAvoid) return false;

        auto away = RandomPos::GetPosAway(*m_mob, 16, 7, m_toAvoid->position);
        if (!away) return false;

        // Reject an "escape" that is actually closer to the threat than the mob
        // already is — the biased roll makes this rare but not impossible.
        if (m_toAvoid->DistanceToSqr(away->x, away->y, away->z) <
            m_toAvoid->DistanceToSqr(*m_mob)) {
            return false;
        }

        m_path = m_mob->GetNavigation().CreatePath(
            glm::ivec3(static_cast<int>(std::floor(away->x)),
                       static_cast<int>(std::floor(away->y)),
                       static_cast<int>(std::floor(away->z))), 0);
        return m_path.has_value();
    }

    // The nearest entity of the kinds this mob flees, or the nearest player
    // when MC named Player.class.
    LivingEntity* AvoidEntityGoal::FindThreat(EntityLevel& level) const {
        if (m_avoidsPlayers) {
            return level.GetNearestPlayer(m_mob->position.x, m_mob->position.y,
                                          m_mob->position.z, m_maxDistance);
        }
        AABB box = m_mob->GetAABB();
        box.min -= glm::vec3(m_maxDistance, 3.0f, m_maxDistance);
        box.max += glm::vec3(m_maxDistance, 3.0f, m_maxDistance);

        std::vector<Entity*> nearby;
        level.GetEntitiesInBox(box, m_mob, nearby);

        LivingEntity* best = nullptr;
        double bestDistSq = 0.0;
        for (Entity* e : nearby) {
            bool wanted = false;
            for (int i = 0; i < m_typeCount; ++i) {
                if (e->GetType() == m_types[i]) { wanted = true; break; }
            }
            if (!wanted) continue;
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            const double d = m_mob->DistanceToSqr(*living);
            if (d > static_cast<double>(m_maxDistance) * m_maxDistance) continue;
            if (!best || d < bestDistSq) { best = living; bestDistSq = d; }
        }
        return best;
    }

    bool AvoidEntityGoal::CanContinueToUse() {
        return !m_mob->GetNavigation().IsDone();
    }

    void AvoidEntityGoal::Start() {
        m_mob->GetNavigation().MoveTo(m_path, m_walkSpeedModifier);
    }

    void AvoidEntityGoal::Stop() {
        m_toAvoid = nullptr;
    }

    void AvoidEntityGoal::Tick() {
        // Sprint only while the threat is within 7 blocks; walk once clear.
        if (!m_toAvoid) return;
        m_mob->GetNavigation().SetSpeedModifier(
            m_mob->DistanceToSqr(*m_toAvoid) < 49.0 ? m_sprintSpeedModifier : m_walkSpeedModifier);
    }


    void SwellGoal::ClearReferenceTo(const Entity* entity) {
        if (m_target == entity) m_target = nullptr;
    }

    void LeapAtTargetGoal::ClearReferenceTo(const Entity* entity) {
        if (m_target == entity) m_target = nullptr;
    }

    void AvoidEntityGoal::ClearReferenceTo(const Entity* entity) {
        if (m_toAvoid == entity) m_toAvoid = nullptr;
    }

} // namespace Game
