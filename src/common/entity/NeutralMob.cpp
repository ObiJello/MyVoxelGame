// File: src/common/entity/NeutralMob.cpp
#include "common/entity/NeutralMob.hpp"

#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"

namespace Game {

    void NeutralMob::SetTimeToRemainAngry(int64_t remainingTicks) {
        EntityLevel* level = m_neutralSelf->Level();
        if (!level) return;
        SetPersistentAngerEndTime(level->GetGameTime() + remainingTicks);
    }

    bool NeutralMob::IsValidPlayerTarget(const LivingEntity& target) {
        // MC: instanceof Player && !creative && !spectator.
        return target.IsPlayer() && !target.IsCreative() && !target.IsSpectator();
    }

    bool NeutralMob::IsAngry() const {
        // MC isAngry: endTime > 0 and the window has not run out yet.
        const int64_t endTime = m_persistentAngerEndTime;
        if (endTime <= 0) return false;
        EntityLevel* level = m_neutralSelf->Level();
        if (!level) return false;
        return endTime - level->GetGameTime() > 0;
    }
    void NeutralMob::SetPersistentAngerTarget(LivingEntity* target) {
        m_angryAtRef.Set(target);
    }

    LivingEntity* NeutralMob::GetPersistentAngerTarget() {
        EntityLevel* level = m_neutralSelf ? m_neutralSelf->Level() : nullptr;
        return level ? m_angryAtRef.GetLiving(*level) : nullptr;
    }


    bool NeutralMob::IsAngryAt(const LivingEntity& entity) const {
        // MC isAngryAt(entity, level), condition for condition.
        if (!m_neutralSelf->CanAttack(entity)) return false;
        if (IsValidPlayerTarget(entity) && IsAngryAtAllPlayers()) return true;
        // A UUID compare, matching NeutralMob.isAngryAt. A pointer compare
        // would mean a mob loaded from disk is not angry at its attacker until
        // that attacker happens to resolve — which is exactly when it matters.
        return m_angryAtRef.Matches(entity);
    }

    void NeutralMob::StopBeingAngry() {
        // MC stopBeingAngry: drop the grudge everywhere it is remembered.
        m_neutralSelf->SetLastHurtByMob(nullptr);
        SetPersistentAngerTarget(nullptr);
        m_neutralSelf->SetTarget(nullptr);
        SetPersistentAngerEndTime(kNoAngerEndTime);
    }

    void NeutralMob::UpdatePersistentAnger(bool stayAngryIfTargetPresent) {
        // MC NeutralMob.updatePersistentAnger, branch for branch. The local
        // `persistentAngerTarget` is captured BEFORE any reassignment, exactly
        // as MC's local is — the "stop being angry with nothing to be angry
        // at" test runs against the value the tick STARTED with.
        LivingEntity* target = m_neutralSelf->GetTarget();
        LivingEntity* persistentAngerTarget = GetPersistentAngerTarget();

        if (target != nullptr && target->IsDeadOrDying() &&
            persistentAngerTarget == target &&
            dynamic_cast<Mob*>(target) != nullptr) {
            // MC: a dead MOB grudge (not a player) is simply dropped.
            StopBeingAngry();
            return;
        }

        if (target != nullptr) {
            if (persistentAngerTarget != target) {
                SetPersistentAngerTarget(target);
            }
            StartPersistentAngerTimer();
        }

        if (persistentAngerTarget != nullptr && !IsAngry() &&
            (target == nullptr || !IsValidPlayerTarget(*target) ||
             !stayAngryIfTargetPresent)) {
            StopBeingAngry();
        }

        // MC: a grudge against a player who went creative or spectator is
        // dropped (isCreative() || isSpectator()).
        if (persistentAngerTarget != nullptr &&
            persistentAngerTarget->IsPlayer() &&
            (persistentAngerTarget->IsCreative() ||
             persistentAngerTarget->IsSpectator())) {
            StopBeingAngry();
        }
    }

} // namespace Game
