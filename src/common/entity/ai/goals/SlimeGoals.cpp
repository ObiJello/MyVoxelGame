// File: src/common/entity/ai/goals/SlimeGoals.cpp
#include "common/entity/ai/goals/SlimeGoals.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"

namespace Game {

    namespace {
        SlimeMoveControl* SlimeControl(Slime& slime) {
            // MC guards every goal on `moveControl instanceof SlimeMoveControl`
            // — a subclass could swap it out.
            return dynamic_cast<SlimeMoveControl*>(&slime.GetMoveControl());
        }
    }

    // ── SlimeFloatGoal ─────────────────────────────────────────────────────

    SlimeFloatGoal::SlimeFloatGoal(Slime* slime) : m_slime(slime) {
        SetFlags(GoalFlag::Jump | GoalFlag::Move);
    }

    bool SlimeFloatGoal::CanUse() {
        return (m_slime->IsInWater() || m_slime->IsInLava()) && SlimeControl(*m_slime);
    }

    void SlimeFloatGoal::Tick() {
        if (m_slime->Level()->Random().NextFloat() < 0.8f) {
            m_slime->GetJumpControl().Jump();
        }
        if (SlimeMoveControl* control = SlimeControl(*m_slime)) {
            control->SetWantedMovement(1.2);
        }
    }

    // ── SlimeAttackGoal ────────────────────────────────────────────────────

    SlimeAttackGoal::SlimeAttackGoal(Slime* slime) : m_slime(slime) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    bool SlimeAttackGoal::CanUse() {
        LivingEntity* target = m_slime->GetTarget();
        if (!target) return false;
        return m_slime->CanAttack(*target) && SlimeControl(*m_slime) != nullptr;
    }

    void SlimeAttackGoal::Start() {
        m_growTiredTimer = ReducedTickDelay(300);
    }

    bool SlimeAttackGoal::CanContinueToUse() {
        LivingEntity* target = m_slime->GetTarget();
        if (!target) return false;
        if (!m_slime->CanAttack(*target)) return false;
        return --m_growTiredTimer > 0;
    }

    void SlimeAttackGoal::Tick() {
        LivingEntity* target = m_slime->GetTarget();
        if (target) {
            m_slime->GetLookControl().SetLookAt(target->position.x, target->GetEyeY(),
                                                target->position.z, 10.0f, 10.0f);
        }
        if (SlimeMoveControl* control = SlimeControl(*m_slime)) {
            // The heading is wherever the look control just turned the body.
            control->SetDirection(m_slime->yRot, m_slime->DealsDamage());
        }
    }

    // ── SlimeRandomDirectionGoal ───────────────────────────────────────────

    SlimeRandomDirectionGoal::SlimeRandomDirectionGoal(Slime* slime) : m_slime(slime) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    bool SlimeRandomDirectionGoal::CanUse() {
        // MC also allows levitation; no status effects exist yet.
        return m_slime->GetTarget() == nullptr &&
               (m_slime->onGround || m_slime->IsInWater() || m_slime->IsInLava()) &&
               SlimeControl(*m_slime) != nullptr;
    }

    void SlimeRandomDirectionGoal::Tick() {
        if (--m_nextRandomizeTime <= 0) {
            JavaRandom& rng = m_slime->Level()->Random();
            m_nextRandomizeTime = AdjustedTickDelay(40 + rng.NextInt(60));
            m_chosenDegrees = static_cast<float>(rng.NextInt(360));
        }
        if (SlimeMoveControl* control = SlimeControl(*m_slime)) {
            control->SetDirection(m_chosenDegrees, false);
        }
    }

    // ── SlimeKeepOnJumpingGoal ─────────────────────────────────────────────

    SlimeKeepOnJumpingGoal::SlimeKeepOnJumpingGoal(Slime* slime) : m_slime(slime) {
        SetFlags(GoalFlag::Jump | GoalFlag::Move);
    }

    bool SlimeKeepOnJumpingGoal::CanUse() {
        // MC: !isPassenger — no riding here, so always.
        return true;
    }

    void SlimeKeepOnJumpingGoal::Tick() {
        if (SlimeMoveControl* control = SlimeControl(*m_slime)) {
            control->SetWantedMovement(1.0);
        }
    }

} // namespace Game
