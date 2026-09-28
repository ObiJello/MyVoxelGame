// File: src/common/entity/ai/goals/TraderGoals.cpp
#include "common/entity/ai/goals/TraderGoals.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/npc/Villager.hpp"

#include <cmath>
#include <vector>

namespace Game {

    // ── TradeWithPlayerGoal ────────────────────────────────────────────────

    TradeWithPlayerGoal::TradeWithPlayerGoal(AbstractVillager* mob) : m_mob(mob) {
        SetFlags(GoalFlag::Jump | GoalFlag::Move);
    }

    bool TradeWithPlayerGoal::CanUse() {
        if (!m_mob->IsAlive()) return false;
        if (m_mob->IsInWater()) return false;
        if (!m_mob->onGround) return false;
        // MC LivingEntity.wasHurtRecently: hurtTime > 0.
        if (m_mob->hurtTime > 0) return false;
        LivingEntity* trader = m_mob->GetTradingPlayer();
        if (!trader) return false;
        return !(m_mob->DistanceToSqr(*trader) > 16.0);
    }

    void TradeWithPlayerGoal::Start() {
        m_mob->GetNavigation().Stop();
    }

    void TradeWithPlayerGoal::Stop() {
        m_mob->SetTradingPlayer(nullptr);
    }

    // ── LookAtTradingPlayerGoal ────────────────────────────────────────────

    LookAtTradingPlayerGoal::LookAtTradingPlayerGoal(AbstractVillager* mob) : m_mob(mob) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    bool LookAtTradingPlayerGoal::CanUse() {
        // MC: no probability roll — while trading, the partner is the target.
        if (!m_mob->IsTrading()) return false;
        m_lookAt = m_mob->GetTradingPlayer();
        return m_lookAt != nullptr;
    }

    bool LookAtTradingPlayerGoal::CanContinueToUse() {
        // MC LookAtPlayerGoal.canContinueToUse.
        if (!m_lookAt || !m_lookAt->IsAlive()) return false;
        if (m_mob->DistanceToSqr(*m_lookAt) >
            static_cast<double>(m_lookDistance) * static_cast<double>(m_lookDistance)) {
            return false;
        }
        return m_lookTime > 0;
    }

    void LookAtTradingPlayerGoal::Start() {
        m_lookTime = AdjustedTickDelay(40 + m_mob->Level()->Random().NextInt(40));
    }

    void LookAtTradingPlayerGoal::Stop() {
        m_lookAt = nullptr;
    }

    void LookAtTradingPlayerGoal::Tick() {
        if (!m_lookAt || !m_lookAt->IsAlive()) return;
        m_mob->GetLookControl().SetLookAt(m_lookAt->position.x, m_lookAt->GetEyeY(),
                                          m_lookAt->position.z);
        --m_lookTime;
    }

    void LookAtTradingPlayerGoal::ClearReferenceTo(const Entity* entity) {
        if (m_lookAt == entity) m_lookAt = nullptr;
    }

    // ── LookAtMobGoal ──────────────────────────────────────────────────────

    LookAtMobGoal::LookAtMobGoal(Mob* mob, float lookDistance, float probability)
        : m_mob(mob), m_lookDistance(lookDistance), m_probability(probability) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
        m_conditions = TargetingConditions::ForNonCombat().Range(lookDistance);
    }

    bool LookAtMobGoal::CanUse() {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        if (level->Random().NextFloat() >= m_probability) return false;

        // MC assigns the attack target first, then overwrites it with the
        // search result (null included) — kept as MC has it.
        m_lookAt = m_mob->GetTarget();

        // getEntitiesOfClass(Mob.class, box.inflate(d, 3, d)) then
        // getNearestEntity(..., lookAtContext, mob, x, eyeY, z).
        AABB box = m_mob->GetAABB();
        box.min -= glm::vec3(m_lookDistance, 3.0f, m_lookDistance);
        box.max += glm::vec3(m_lookDistance, 3.0f, m_lookDistance);
        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_mob, nearby);

        const double eyeY = m_mob->GetEyeY();
        LivingEntity* nearest = nullptr;
        double nearestDistSq = -1.0;
        for (Entity* e : nearby) {
            auto* mob = dynamic_cast<Mob*>(e);
            // Mob.class — a real mob, not one of the engine's pipeline
            // riders (hanging entities, projectiles, primed TNT), which are
            // Mobs here only because the tracker is Mob-shaped.
            if (!mob || !mob->HasAiControls() || !mob->IsAlive()) continue;
            if (!m_conditions.Test(m_mob, *mob)) continue;
            const double dx = mob->position.x - m_mob->position.x;
            const double dy = mob->position.y - eyeY;
            const double dz = mob->position.z - m_mob->position.z;
            const double d = dx * dx + dy * dy + dz * dz;
            if (nearestDistSq == -1.0 || d < nearestDistSq) {
                nearestDistSq = d;
                nearest = mob;
            }
        }
        m_lookAt = nearest;
        return m_lookAt != nullptr;
    }

    bool LookAtMobGoal::CanContinueToUse() {
        if (!m_lookAt || !m_lookAt->IsAlive()) return false;
        if (m_mob->DistanceToSqr(*m_lookAt) >
            static_cast<double>(m_lookDistance) * static_cast<double>(m_lookDistance)) {
            return false;
        }
        return m_lookTime > 0;
    }

    void LookAtMobGoal::Start() {
        m_lookTime = AdjustedTickDelay(40 + m_mob->Level()->Random().NextInt(40));
    }

    void LookAtMobGoal::Stop() {
        m_lookAt = nullptr;
    }

    void LookAtMobGoal::Tick() {
        if (!m_lookAt || !m_lookAt->IsAlive()) return;
        m_mob->GetLookControl().SetLookAt(m_lookAt->position.x, m_lookAt->GetEyeY(),
                                          m_lookAt->position.z);
        --m_lookTime;
    }

    void LookAtMobGoal::ClearReferenceTo(const Entity* entity) {
        if (m_lookAt == entity) m_lookAt = nullptr;
    }

    // ── TraderUseItemGoal ──────────────────────────────────────────────────

    TraderUseItemGoal::TraderUseItemGoal(WanderingTrader* mob, WanderingTrader::HeldItem item,
                                         const char* finishUsingSound, Selector canUseSelector)
        : m_mob(mob), m_item(item), m_finishUsingSound(finishUsingSound),
          m_canUseSelector(canUseSelector) {
        // MC UseItemGoal sets no flags: it runs beside movement and looking.
    }

    bool TraderUseItemGoal::CanUse() {
        return m_canUseSelector && m_canUseSelector(*m_mob);
    }

    bool TraderUseItemGoal::CanContinueToUse() {
        return m_mob->IsUsingItem();
    }

    void TraderUseItemGoal::Start() {
        // MC: setItemSlot(MAINHAND, item.copy()); startUsingItem(MAIN_HAND).
        m_mob->SetHeldItem(m_item);
        m_mob->StartUsingItem();
    }

    void TraderUseItemGoal::Stop() {
        m_mob->SetHeldItem(WanderingTrader::HeldItem::None);
        if (m_finishUsingSound && m_mob->Level()) {
            const float pitch = m_mob->Level()->Random().NextFloat() * 0.2f + 0.9f;
            m_mob->PlaySound(m_finishUsingSound, 1.0f, pitch);
        }
    }

    // ── WanderToPositionGoal ───────────────────────────────────────────────

    WanderToPositionGoal::WanderToPositionGoal(WanderingTrader* trader, double stopDistance,
                                               double speedModifier)
        : m_trader(trader), m_stopDistance(stopDistance), m_speedModifier(speedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool WanderToPositionGoal::IsTooFarAway(const glm::ivec3& pos, double distance) const {
        // MC !BlockPos.closerToCenterThan(position, distance): the distance
        // from the BLOCK CENTRE, squared, against distance².
        const double dx = m_trader->position.x - (pos.x + 0.5);
        const double dy = m_trader->position.y - (pos.y + 0.5);
        const double dz = m_trader->position.z - (pos.z + 0.5);
        return !(dx * dx + dy * dy + dz * dz < distance * distance);
    }

    bool WanderToPositionGoal::CanUse() {
        const std::optional<glm::ivec3>& target = m_trader->GetWanderTarget();
        return target && IsTooFarAway(*target, m_stopDistance);
    }

    void WanderToPositionGoal::Stop() {
        m_trader->SetWanderTarget(std::nullopt);
        m_trader->GetNavigation().Stop();
    }

    void WanderToPositionGoal::Tick() {
        const std::optional<glm::ivec3> target = m_trader->GetWanderTarget();
        if (!target || !m_trader->GetNavigation().IsDone()) return;
        if (IsTooFarAway(*target, 10.0)) {
            // A 10-block leg straight toward the target.
            glm::dvec3 dir(static_cast<double>(target->x) - m_trader->position.x,
                           static_cast<double>(target->y) - m_trader->position.y,
                           static_cast<double>(target->z) - m_trader->position.z);
            const double len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            // MC Vec3.normalize: a vector shorter than 1e-5 becomes ZERO.
            dir = len < 1.0e-5 ? glm::dvec3(0.0) : dir / len;
            const glm::dvec3 leg = dir * 10.0 + m_trader->position;
            m_trader->GetNavigation().MoveTo(leg.x, leg.y, leg.z, m_speedModifier);
        } else {
            m_trader->GetNavigation().MoveTo(static_cast<double>(target->x),
                                             static_cast<double>(target->y),
                                             static_cast<double>(target->z), m_speedModifier);
        }
    }

    // ── TraderLlamaDefendWanderingTraderGoal ───────────────────────────────

    TraderLlamaDefendWanderingTraderGoal::TraderLlamaDefendWanderingTraderGoal(Llama* llama)
        : TargetGoal(llama, /*mustSee=*/false), m_llama(llama) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Target));
    }

    bool TraderLlamaDefendWanderingTraderGoal::CanUse() {
        if (!m_llama->IsLeashed()) return false;
        auto* owner = dynamic_cast<WanderingTrader*>(m_llama->GetLeashHolder());
        if (!owner) return false;
        m_ownerLastHurtBy = dynamic_cast<LivingEntity*>(owner->GetLastHurtByMob());
        const int64_t ts = owner->GetLastHurtByMobTimestamp();
        if (ts == m_timestamp) return false;
        // MC TargetGoal.canAttack(target, TargetingConditions.DEFAULT): the
        // combat conditions (alive, attackable, in sight) and the llama's
        // home restriction.
        LivingEntity* target = m_ownerLastHurtBy;
        if (!target || !target->IsAlive()) return false;
        if (!TargetingConditions::ForCombat().Test(m_mob, *target)) return false;
        return m_mob->IsWithinHome(target->BlockPosition());
    }

    void TraderLlamaDefendWanderingTraderGoal::Start() {
        m_mob->SetTarget(m_ownerLastHurtBy);
        m_targetMob = m_ownerLastHurtBy;
        if (auto* owner = dynamic_cast<WanderingTrader*>(m_llama->GetLeashHolder())) {
            m_timestamp = owner->GetLastHurtByMobTimestamp();
        }
        TargetGoal::Start();
    }

    void TraderLlamaDefendWanderingTraderGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_ownerLastHurtBy == entity) m_ownerLastHurtBy = nullptr;
    }

} // namespace Game
