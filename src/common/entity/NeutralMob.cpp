// File: src/common/entity/NeutralMob.cpp
#include "common/entity/NeutralMob.hpp"
#include "common/world/level/GameRules.hpp"

#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"

namespace Game {

    void NeutralMob::SetTimeToRemainAngry(int64_t remainingTicks) {
        EntityLevel* level = m_neutralSelf->Level();
        if (!level) return;
        SetPersistentAngerEndTime(level->GetGameTime() + remainingTicks);
    }

    bool NeutralMob::IsValidPlayerTarget(const LivingEntity& target) {
        // MC: instanceof Player && !creative && !spectator && the player's
        // level is not on Peaceful.
        if (!target.IsPlayer() || target.IsCreative() || target.IsSpectator()) return false;
        const EntityLevel* level = target.Level();
        return !level || level->GetDifficulty() != Difficulty::Peaceful;
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


    bool NeutralMob::IsAngryAtAllPlayers() const {
        return Rules::GetBool(Rules::Id::UniversalAnger) && IsAngry() && m_angryAtRef.Empty();
    }

    void NeutralMob::PlayerDied(const LivingEntity& player) {
        if (!Rules::GetBool(Rules::Id::ForgiveDeadPlayers)) return;
        if (m_angryAtRef.Matches(player)) StopBeingAngry();
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
        // MC NeutralMob.updatePersistentAnger, branch for branch. MC's local
        // `persistentAngerTarget` is the EntityReference the tick STARTED
        // with — an identity, not a resolved entity — so every test below
        // that MC makes against it is made against the reference: a grudge
        // against a player who is offline (unresolved) still expires, and
        // "is this a new target" is a UUID compare, not a pointer one.
        LivingEntity* previousTarget = m_neutralSelf->GetTarget();   // getTargetUnchecked
        const EntityRef startRef = m_angryAtRef;
        const bool hadAngerTarget = !startRef.Empty();

        if (previousTarget != nullptr && previousTarget->IsDeadOrDying() &&
            hadAngerTarget && startRef.Matches(*previousTarget) &&
            dynamic_cast<Mob*>(previousTarget) != nullptr) {
            // MC: a dead MOB grudge (not a player) is simply dropped.
            StopBeingAngry();
            return;
        }

        LivingEntity* target = m_neutralSelf->GetTarget();
        if (target != nullptr) {
            const bool newTarget = !hadAngerTarget || !startRef.Matches(*target);
            if (newTarget) SetPersistentAngerTarget(target);
            if (newTarget || stayAngryIfTargetPresent) StartPersistentAngerTimer();
        }

        if (hadAngerTarget && !IsAngry() &&
            (target == nullptr || !IsValidPlayerTarget(*target) ||
             !stayAngryIfTargetPresent)) {
            StopBeingAngry();
        }

        // MC: a grudge (the reference the tick started with) against a player
        // who went creative or spectator — or any grudge on Peaceful — is
        // dropped. The reference is resolved here, as MC's
        // EntityReference.getLivingEntity is; an offline player resolves to
        // nothing and keeps the grudge.
        EntityLevel* level = m_neutralSelf->Level();
        if (hadAngerTarget && level) {
            EntityRef resolver = startRef;
            LivingEntity* persistentTarget = resolver.GetLiving(*level);
            if (persistentTarget != nullptr && persistentTarget->IsPlayer() &&
                (persistentTarget->IsCreative() || persistentTarget->IsSpectator() ||
                 level->GetDifficulty() == Difficulty::Peaceful)) {
                StopBeingAngry();
            }
        }
    }

} // namespace Game
