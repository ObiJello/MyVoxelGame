// File: src/common/entity/ai/goals/GuardianGoals.cpp
#include "common/entity/ai/goals/GuardianGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/core/JavaRandom.hpp"

#include <vector>

namespace Game {

    // ── GuardianAttackGoal ─────────────────────────────────────────────────

    GuardianAttackGoal::GuardianAttackGoal(Guardian* guardian)
        : m_guardian(guardian),
          m_elder(guardian->GetType() == EntityTypeId::ElderGuardian) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool GuardianAttackGoal::CanUse() {
        LivingEntity* target = m_guardian->GetTarget();
        return target != nullptr && target->IsAlive();
    }

    bool GuardianAttackGoal::CanContinueToUse() {
        // MC: super.canContinueToUse() is canUse(); the elder holds its beam
        // at any range, the plain guardian drops a target inside 3 blocks
        // (matching the selector's distSqr > 9 acquisition rule).
        return CanUse() &&
               (m_elder ||
                (m_guardian->GetTarget() != nullptr &&
                 m_guardian->DistanceToSqr(*m_guardian->GetTarget()) > 9.0));
    }

    void GuardianAttackGoal::Start() {
        // MC: attackTime starts at -10 — ten free ticks of staring before the
        // synced charge (and the client's beam clock) begins at 0.
        m_attackTime = -10;
        m_guardian->GetNavigation().Stop();
        LivingEntity* target = m_guardian->GetTarget();
        if (target != nullptr) {
            m_guardian->GetLookControl().SetLookAt(
                target->position.x, target->GetEyeY(), target->position.z,
                90.0f, 90.0f);
        }
        m_guardian->needsSync = true;
    }

    void GuardianAttackGoal::Stop() {
        // MC: clear the synced beam target, drop the target, and send the
        // guardian wandering.
        m_guardian->SetActiveAttackTarget(false);
        m_guardian->SetTarget(nullptr);
        m_guardian->TriggerRandomStroll();
    }

    void GuardianAttackGoal::Tick() {
        LivingEntity* target = m_guardian->GetTarget();
        if (target == nullptr) return;

        m_guardian->GetNavigation().Stop();
        m_guardian->GetLookControl().SetLookAt(
            target->position.x, target->GetEyeY(), target->position.z,
            90.0f, 90.0f);

        // MC: losing line of sight cancels the charge outright — the beam
        // does not track through walls.
        if (!m_guardian->GetSensing().HasLineOfSight(*target)) {
            m_guardian->SetTarget(nullptr);
            return;
        }

        ++m_attackTime;
        if (m_attackTime == 0) {
            // MC: sync DATA_ID_ATTACK_TARGET (the boolean half here — the
            // beam visual's entity id is the skipped render piece) and
            // broadcast entity event 21 (the attack sound — no sound system).
            m_guardian->SetActiveAttackTarget(true);
            if (m_guardian->Level()) {
                m_guardian->Level()->BroadcastEntityEvent(*m_guardian, 21);
            }
        } else if (m_attackTime >= m_guardian->GetAttackDuration()) {
            // MC's damage math, verbatim: 1 base, +2 on HARD, +2 for the
            // elder, dealt as indirect magic; then the ordinary melee hit on
            // top (ATTACK_DAMAGE 6 / elder 8), then the target is dropped so
            // a fresh charge must begin.
            float magicDamage = 1.0f;
            if (m_guardian->Level() &&
                m_guardian->Level()->GetDifficulty() == Difficulty::Hard) {
                magicDamage += 2.0f;
            }
            if (m_elder) {
                magicDamage += 2.0f;
            }

            target->Hurt(MobDamageSource::Magic, magicDamage, m_guardian);
            m_guardian->DoHurtTarget(*target);
            m_guardian->SetTarget(nullptr);
        }
    }

    // ── GuardianAttackTargetGoal ───────────────────────────────────────────

    GuardianAttackTargetGoal::GuardianAttackTargetGoal(Guardian* guardian)
        : TargetGoal(guardian, /*mustSee=*/true, /*mustReach=*/false),
          m_guardian(guardian),
          m_randomInterval(ReducedTickDelay(10)) {
        m_conditions = TargetingConditions::ForCombat();
    }

    void GuardianAttackTargetGoal::FindTarget() {
        m_target = nullptr;
        EntityLevel* level = m_guardian->Level();
        if (!level) return;

        const double follow = GetFollowDistance();
        m_conditions.range = follow;

        // MC GuardianAttackSelector: (Player || Squid || Axolotl) &&
        // distSqr > 9. GlowSquid extends Squid in MC and counts.
        AABB box = m_guardian->GetAABB();
        box.min -= glm::vec3(follow, 4.0, follow);
        box.max += glm::vec3(follow, 4.0, follow);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_guardian, nearby);

        LivingEntity* best = nullptr;
        double bestDistSq = 0.0;
        auto consider = [&](LivingEntity* living) {
            if (!living || !m_conditions.Test(m_guardian, *living)) return;
            const double d = m_guardian->DistanceToSqr(*living);
            if (d <= 9.0) return;
            if (!best || d < bestDistSq) { best = living; bestDistSq = d; }
        };

        for (Entity* e : nearby) {
            const EntityTypeId type = e->GetType();
            if (type != EntityTypeId::Squid && type != EntityTypeId::GlowSquid &&
                type != EntityTypeId::Axolotl) {
                continue;
            }
            consider(dynamic_cast<LivingEntity*>(e));
        }
        // Players are not in the entity box query; ask the level directly and
        // apply the same selector.
        std::vector<LivingEntity*> players;
        level->GetPlayers(players);
        for (LivingEntity* player : players) {
            if (!player) continue;
            if (m_guardian->DistanceToSqr(*player) > follow * follow) continue;
            consider(player);
        }

        m_target = best;
    }

    bool GuardianAttackTargetGoal::CanUse() {
        // MC NearestAttackableTargetGoal's 1-in-(interval/2) search gate.
        if (m_randomInterval > 0 && m_guardian->Level() &&
            m_guardian->Level()->Random().NextInt(m_randomInterval) != 0) {
            return false;
        }
        FindTarget();
        return m_target != nullptr;
    }

    void GuardianAttackTargetGoal::Start() {
        m_guardian->SetTarget(m_target);
        m_targetMob = m_target;
        TargetGoal::Start();
    }

    void GuardianAttackTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_target == entity) m_target = nullptr;
    }

} // namespace Game
