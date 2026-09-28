// File: src/common/entity/ai/goals/TamableGoals.cpp
#include "common/entity/ai/goals/TamableGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/TamableAnimal.hpp"
// BegGoal is typed on the Wolf (its interested flag + food list) and reads
// the held-item id — the one goal in this file that needs the concrete class.
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"

namespace Game {

    // ── SitWhenOrderedToGoal ───────────────────────────────────────────────

    SitWhenOrderedToGoal::SitWhenOrderedToGoal(Mob* mob, TamableAnimal* tamable)
        : m_mob(mob), m_tamable(tamable) {
        SetFlags(GoalFlag::Jump | GoalFlag::Move);
    }

    bool SitWhenOrderedToGoal::CanContinueToUse() {
        return m_tamable->IsOrderedToSit();
    }

    bool SitWhenOrderedToGoal::CanUse() {
        const bool orderedToSit = m_tamable->IsOrderedToSit();
        if (!orderedToSit && !m_tamable->IsTame()) return false;
        if (m_mob->IsInWater()) return false;
        if (!m_mob->onGround) return false;

        LivingEntity* owner = m_tamable->GetOwner();
        // MC: no owner, or one in another level → sit anyway.
        if (!owner || owner->Level() != m_mob->Level()) return true;
        // MC: within 12 blocks of an owner who is under attack → refuse, so
        // the pet stands up to defend rather than sitting through the fight.
        return m_mob->DistanceToSqr(*owner) < 144.0 &&
                       owner->GetLastHurtByMob() != nullptr
                   ? false
                   : orderedToSit;
    }

    void SitWhenOrderedToGoal::Start() {
        m_mob->GetNavigation().Stop();
        m_tamable->SetInSittingPose(true);
    }

    void SitWhenOrderedToGoal::Stop() {
        m_tamable->SetInSittingPose(false);
    }

    // ── FollowOwnerGoal ────────────────────────────────────────────────────

    FollowOwnerGoal::FollowOwnerGoal(Mob* mob, TamableAnimal* tamable,
                                     double speedModifier, float startDistance,
                                     float stopDistance)
        : m_mob(mob), m_tamable(tamable), m_speedModifier(speedModifier),
          m_startDistance(startDistance), m_stopDistance(stopDistance) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool FollowOwnerGoal::CanUse() {
        LivingEntity* owner = m_tamable->GetOwner();
        if (!owner) return false;
        if (m_tamable->UnableToMoveToOwner()) return false;
        if (m_mob->DistanceToSqr(*owner) <
            static_cast<double>(m_startDistance) * m_startDistance) {
            return false;
        }
        m_owner = owner;
        return true;
    }

    bool FollowOwnerGoal::CanContinueToUse() {
        if (m_mob->GetNavigation().IsDone()) return false;
        if (m_tamable->UnableToMoveToOwner()) return false;
        return !(m_owner && m_mob->DistanceToSqr(*m_owner) <=
                                static_cast<double>(m_stopDistance) * m_stopDistance);
    }

    void FollowOwnerGoal::Start() {
        m_timeToRecalcPath = 0;
        // MC zeroes the WATER malus for the duration so the follower swims
        // straight across instead of pathing around the pond.
        m_oldWaterCost = m_mob->GetPathfindingMalus(PathType::Water);
        m_mob->SetPathfindingMalus(PathType::Water, 0.0f);
    }

    void FollowOwnerGoal::Stop() {
        m_owner = nullptr;
        m_mob->GetNavigation().Stop();
        m_mob->SetPathfindingMalus(PathType::Water, m_oldWaterCost);
    }

    void FollowOwnerGoal::Tick() {
        if (!m_owner) return;
        const bool ownerFarAway = m_tamable->ShouldTryTeleportToOwner();
        if (!ownerFarAway) {
            const glm::dvec3 eye = m_owner->GetEyePosition();
            m_mob->GetLookControl().SetLookAt(
                eye.x, eye.y, eye.z, 10.0f,
                static_cast<float>(m_mob->GetMaxHeadXRot()));
        }

        if (--m_timeToRecalcPath <= 0) {
            m_timeToRecalcPath = AdjustedTickDelay(10);
            if (ownerFarAway) {
                m_tamable->TryToTeleportToOwner();
            } else {
                m_mob->GetNavigation().MoveTo(*m_owner, m_speedModifier);
            }
        }
    }

    void FollowOwnerGoal::ClearReferenceTo(const Entity* entity) {
        if (m_owner == entity) m_owner = nullptr;
    }

    // ── TamableAnimalPanicGoal ─────────────────────────────────────────────

    TamableAnimalPanicGoal::TamableAnimalPanicGoal(PathfinderMob* mob,
                                                   TamableAnimal* tamable,
                                                   double speedModifier,
                                                   Causes causes)
        : PanicGoal(mob, speedModifier), m_tamable(tamable), m_causes(causes) {}

    bool TamableAnimalPanicGoal::ShouldPanic() const {
        if (m_causes == Causes::All) return PanicGoal::ShouldPanic();
        // MC shouldPanic against #panic_environmental_causes. Of that tag
        // (cactus, freeze, hot_floor, in_fire, lava, lightning_bolt,
        // on_fire) this engine's damage sources name fire and lava; it has no
        // cactus, freeze, hot-floor or lightning damage type of its own.
        if (!m_mob->HasLastDamageSource()) return false;
        const MobDamageSource source = m_mob->GetLastDamageSource();
        return source == MobDamageSource::Fire || source == MobDamageSource::Lava;
    }

    void TamableAnimalPanicGoal::Tick() {
        if (!m_tamable->UnableToMoveToOwner() &&
            m_tamable->ShouldTryTeleportToOwner()) {
            m_tamable->TryToTeleportToOwner();
        }
        PanicGoal::Tick();
    }

    // ── The owner-defence target pair ──────────────────────────────────────

    namespace {

        // MC TargetGoal.canAttack(target, TargetingConditions.DEFAULT).
        // DEFAULT is a bare forCombat(): line of sight and the combat rules,
        // but NO range — the owner's foe is taken on however far away it is
        // (TargetGoal.canContinueToUse then drops it past the follow range),
        // and the owner's timestamp is spent either way.
        bool CanAttackOwnerFoe(Mob* mob, LivingEntity* target) {
            if (!target || !target->IsAlive()) return false;
            return TargetingConditions::ForCombat().Test(mob, *target);
        }

    } // namespace

    OwnerHurtByTargetGoal::OwnerHurtByTargetGoal(Mob* mob, TamableAnimal* tamable)
        : TargetGoal(mob, /*mustSee=*/false), m_tamable(tamable) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Target));
    }

    bool OwnerHurtByTargetGoal::CanUse() {
        if (!m_tamable->IsTame() || m_tamable->IsOrderedToSit()) return false;
        LivingEntity* owner = m_tamable->GetOwner();
        if (!owner) return false;
        // MC: owner.getLastDamageSource(100) must be live and not in
        // #no_wolf_retaliation. That tag's one member is sulfur_cube_hot,
        // which this engine deals as Fire with the hot sulfur cube as the
        // attacker (SulfurCube::ApplyContactDamage) — so a burn from a cube
        // is the one blow the pet lets go.
        if (!owner->HasLastDamageSourceWithin(100)) return false;
        m_ownerLastHurtBy = dynamic_cast<LivingEntity*>(owner->GetLastHurtByMob());
        if (owner->GetLastDamageSource() == MobDamageSource::Fire && m_ownerLastHurtBy &&
            m_ownerLastHurtBy->GetType() == EntityTypeId::SulfurCube) {
            return false;
        }
        const int64_t ts = owner->GetLastHurtByMobTimestamp();
        return ts != m_timestamp &&
               CanAttackOwnerFoe(m_mob, m_ownerLastHurtBy) &&
               m_tamable->WantsToAttack(*m_ownerLastHurtBy, *owner);
    }

    void OwnerHurtByTargetGoal::Start() {
        m_mob->SetTarget(m_ownerLastHurtBy);
        m_targetMob = m_ownerLastHurtBy;
        if (LivingEntity* owner = m_tamable->GetOwner()) {
            m_timestamp = owner->GetLastHurtByMobTimestamp();
        }
        TargetGoal::Start();
    }

    void OwnerHurtByTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_ownerLastHurtBy == entity) m_ownerLastHurtBy = nullptr;
    }

    OwnerHurtTargetGoal::OwnerHurtTargetGoal(Mob* mob, TamableAnimal* tamable)
        : TargetGoal(mob, /*mustSee=*/false), m_tamable(tamable) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Target));
    }

    bool OwnerHurtTargetGoal::CanUse() {
        if (!m_tamable->IsTame() || m_tamable->IsOrderedToSit()) return false;
        LivingEntity* owner = m_tamable->GetOwner();
        if (!owner) return false;
        m_ownerLastHurt = dynamic_cast<LivingEntity*>(owner->GetLastHurtMob());
        const int64_t ts = owner->GetLastHurtMobTimestamp();
        return ts != m_timestamp &&
               CanAttackOwnerFoe(m_mob, m_ownerLastHurt) &&
               m_tamable->WantsToAttack(*m_ownerLastHurt, *owner);
    }

    void OwnerHurtTargetGoal::Start() {
        m_mob->SetTarget(m_ownerLastHurt);
        m_targetMob = m_ownerLastHurt;
        if (LivingEntity* owner = m_tamable->GetOwner()) {
            m_timestamp = owner->GetLastHurtMobTimestamp();
        }
        TargetGoal::Start();
    }

    void OwnerHurtTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_ownerLastHurt == entity) m_ownerLastHurt = nullptr;
    }

    // ── BegGoal ────────────────────────────────────────────────────────────

    BegGoal::BegGoal(Wolf* wolf, float lookDistance)
        : m_wolf(wolf), m_lookDistance(lookDistance) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    bool BegGoal::CanUse() {
        m_player = m_wolf->Level()
            ? m_wolf->Level()->GetNearestPlayer(
                  m_wolf->position.x, m_wolf->position.y, m_wolf->position.z,
                  static_cast<double>(m_lookDistance))
            : nullptr;
        return m_player != nullptr && PlayerHoldingInteresting();
    }

    bool BegGoal::CanContinueToUse() {
        if (!m_player || !m_player->IsAlive()) return false;
        if (m_wolf->DistanceToSqr(*m_player) >
            static_cast<double>(m_lookDistance) * m_lookDistance) {
            return false;
        }
        return m_lookTime > 0 && PlayerHoldingInteresting();
    }

    void BegGoal::Start() {
        m_wolf->SetIsInterested(true);
        m_lookTime = AdjustedTickDelay(
            40 + m_wolf->Level()->Random().NextInt(40));
    }

    void BegGoal::ClearReferenceTo(const Entity* entity) {
        if (m_player && static_cast<const Entity*>(m_player) == entity) m_player = nullptr;
    }

    void BegGoal::Stop() {
        m_wolf->SetIsInterested(false);
        m_player = nullptr;
    }

    void BegGoal::Tick() {
        if (!m_player) return;
        m_wolf->GetLookControl().SetLookAt(
            m_player->position.x, m_player->GetEyeY(), m_player->position.z,
            10.0f, static_cast<float>(m_wolf->GetMaxHeadXRot()));
        --m_lookTime;
    }

    bool BegGoal::PlayerHoldingInteresting() const {
        if (!m_wolf->Level() || !m_player) return false;
        const uint32_t held = m_wolf->Level()->GetHeldItemId(*m_player);
        return held == Items::Bone || m_wolf->IsFood(held);
    }

} // namespace Game
