// File: src/common/entity/ai/goals/TargetGoals.cpp
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/NeutralMob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
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

        // MC TargetGoal.canAttack: an attacker OUTSIDE the mob's home
        // restriction is not pursued — the retaliation leash.
        if (!m_mob->IsWithinHome(living->BlockPosition())) return false;

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

        // MC's alert box: AABB.unitCubeFromLowerCorner(position) inflated by
        // (followRange, 10, followRange) — a UNIT cube at the mob's feet, not
        // its bounding box, so the reach does not grow with the mob's size.
        const double follow = GetFollowDistance();
        AABB box;
        box.min = glm::vec3(m_mob->position);
        box.max = box.min + glm::vec3(1.0f);
        box.min -= glm::vec3(follow, 10.0, follow);
        box.max += glm::vec3(follow, 10.0, follow);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_mob, nearby);

        for (Entity* e : nearby) {
            Mob* other = dynamic_cast<Mob*>(e);
            if (!other) continue;
            // MC gathers mob.getClass() then filters toIgnoreAlert; the
            // configured predicate carries both halves. Default: same exact
            // type. Only mobs not already busy — MC will not steal a target a
            // mob has already chosen.
            if (m_alertFilter) {
                if (!m_alertFilter(*m_mob, *other)) continue;
            } else if (other->GetType() != m_mob->GetType()) {
                continue;
            }
            if (other->GetTarget()) continue;
            AlertOther(*other, *attacker);
        }
    }

    void HurtByTargetGoal::AlertOther(Mob& other, LivingEntity& attacker) {
        // MC HurtByTargetGoal.alertOther — the per-mob hand-off, split out so
        // the polar bear can filter who gets woken.
        other.SetTarget(&attacker);
    }

    // ── PolarBearHurtByTargetGoal ──────────────────────────────────────────

    PolarBearHurtByTargetGoal::PolarBearHurtByTargetGoal(PolarBear* bear)
        : HurtByTargetGoal(bear) {}

    void PolarBearHurtByTargetGoal::Start() {
        // MC PolarBear.PolarBearHurtByTargetGoal.start: normal retaliation,
        // except a CUB never fights — it wakes the adults and stands down.
        HurtByTargetGoal::Start();
        if (m_mob->IsBaby()) {
            AlertOthers();
            Stop();
        }
    }

    void PolarBearHurtByTargetGoal::AlertOther(Mob& other, LivingEntity& attacker) {
        // MC: only adult polar bears answer the alarm.
        if (dynamic_cast<PolarBear*>(&other) != nullptr && !other.IsBaby()) {
            HurtByTargetGoal::AlertOther(other, attacker);
        }
    }

    // ── PolarBearAttackPlayersGoal ─────────────────────────────────────────

    PolarBearAttackPlayersGoal::PolarBearAttackPlayersGoal(PolarBear* bear)
        : NearestAttackableTargetGoal(bear, /*mustSee=*/true, /*mustReach=*/true,
                                      /*randomInterval=*/20) {}

    bool PolarBearAttackPlayersGoal::CanUse() {
        // MC PolarBear.PolarBearAttackPlayersGoal.canUse: cubs never hunt,
        // and an adult only turns on a player while a CUB is inside the
        // 8x4x8 protection box.
        if (m_mob->IsBaby()) return false;
        if (NearestAttackableTargetGoal::CanUse()) {
            EntityLevel* level = m_mob->Level();
            if (level) {
                AABB box = m_mob->GetAABB();
                box.min -= glm::vec3(8.0f, 4.0f, 8.0f);
                box.max += glm::vec3(8.0f, 4.0f, 8.0f);
                std::vector<Entity*> nearby;
                // Excluding self is safe: the caller is an adult, never the
                // cub being looked for.
                level->GetEntitiesInBox(box, m_mob, nearby);
                for (Entity* e : nearby) {
                    if (e->GetType() == EntityTypeId::PolarBear && e->IsBaby()) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    double PolarBearAttackPlayersGoal::GetFollowDistance() const {
        // MC: half the usual follow distance — the family is defended, not
        // the whole tundra.
        return NearestAttackableTargetGoal::GetFollowDistance() * 0.5;
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
            m_target = (nearest && m_conditions.Test(m_mob, *nearest) &&
                        (!m_selector || m_selector(*m_mob, *nearest)))
                           ? nearest
                           : nullptr;
            return;
        }

        // MC NearestAttackableTargetGoal.getTargetSearchArea: the bounding box
        // inflated by the follow range on ALL axes (1.20.2 dropped the old
        // 4-block vertical clamp — a zombie DOES now notice a villager two
        // floors up).
        AABB box = m_mob->GetAABB();
        box.min -= glm::vec3(follow, follow, follow);
        box.max += glm::vec3(follow, follow, follow);

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
            if (m_selector && !m_selector(*m_mob, *living)) continue;

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
        // MC start(): setTarget + super only. Deliberately NOT cached into
        // m_targetMob — that cache is HurtByTargetGoal's; caching here let
        // CanContinueToUse resurrect a target something else had cleared.
        m_mob->SetTarget(m_target);
        TargetGoal::Start();
    }

    void TargetGoal::ClearReferenceTo(const Entity* entity) {
        if (m_targetMob == entity) m_targetMob = nullptr;
    }

    void NearestAttackableTargetGoal::ClearReferenceTo(const Entity* entity) {
        if (m_target == entity) m_target = nullptr;
    }

    // ── SpiderTargetGoal ───────────────────────────────────────────────────

    bool SpiderTargetGoal::CanUse() {
        // MC: a spider in bright light does not even look for targets.
        if (!Spider::IsDarkEnoughToHunt(*m_mob)) return false;
        return NearestAttackableTargetGoal::CanUse();
    }

    // ── LlamaHurtByTargetGoal ──────────────────────────────────────────────

    LlamaHurtByTargetGoal::LlamaHurtByTargetGoal(Llama* llama)
        : HurtByTargetGoal(llama), m_llama(llama) {}

    bool LlamaHurtByTargetGoal::CanContinueToUse() {
        // MC: one spit settles the grudge.
        if (m_llama->DidSpit()) {
            m_llama->SetDidSpit(false);
            return false;
        }
        return HurtByTargetGoal::CanContinueToUse();
    }

    // ── LlamaAttackWolfGoal ────────────────────────────────────────────────

    namespace {
        const EntityTypeId kWolfTargetList[] = { EntityTypeId::Wolf };
    }

    LlamaAttackWolfGoal::LlamaAttackWolfGoal(Llama* llama)
        : NearestAttackableTargetGoal(llama, kWolfTargetList, 1,
                                      /*mustSee=*/false, /*mustReach=*/true,
                                      /*randomInterval=*/16) {}

    double LlamaAttackWolfGoal::GetFollowDistance() const {
        return NearestAttackableTargetGoal::GetFollowDistance() * 0.25;
    }

    // ── ShulkerNearestAttackGoal ───────────────────────────────────────────

    ShulkerNearestAttackGoal::ShulkerNearestAttackGoal(Shulker* shulker)
        : NearestAttackableTargetGoal(shulker, /*mustSee=*/true),
          m_shulker(shulker) {}

    bool ShulkerNearestAttackGoal::CanUse() {
        if (m_mob->Level() &&
            m_mob->Level()->GetDifficulty() == Difficulty::Peaceful) {
            return false;
        }
        return NearestAttackableTargetGoal::CanUse();
    }

    // ── ShulkerDefenseAttackGoal ───────────────────────────────────────────

    ShulkerDefenseAttackGoal::ShulkerDefenseAttackGoal(Shulker* shulker)
        : NearestAttackableTargetGoal(shulker, /*mustSee=*/true) {}

    // ── ResetUniversalAngerTargetGoal ──────────────────────────────────────

    ResetUniversalAngerTargetGoal::ResetUniversalAngerTargetGoal(
            Mob* mob, bool alertOthersOfSameType)
        : m_mob(mob),
          m_neutral(dynamic_cast<NeutralMob*>(mob)),
          m_alertOthersOfSameType(alertOthersOfSameType) {}

    bool ResetUniversalAngerTargetGoal::WasHurtByPlayer() const {
        // MC wasHurtByPlayer: the last attacker is a player, and this is a
        // NEW hit (timestamp advanced past the one already handled).
        Entity* attacker = m_mob->GetLastHurtByMob();
        return attacker != nullptr && attacker->IsPlayer() &&
               m_mob->GetLastHurtByMobTimestamp() > m_lastHurtByPlayerTimestamp;
    }

    bool ResetUniversalAngerTargetGoal::CanUse() {
        // MC gates on the UNIVERSAL_ANGER game rule, which defaults OFF —
        // see the constant's note in the header.
        return kUniversalAnger && m_neutral != nullptr && WasHurtByPlayer();
    }

    void ResetUniversalAngerTargetGoal::Start() {
        // MC start(): swap the specific grudge for universal anger, and
        // optionally spread it through every same-type neighbour.
        m_lastHurtByPlayerTimestamp = m_mob->GetLastHurtByMobTimestamp();
        m_neutral->ForgetCurrentTargetAndRefreshUniversalAnger();

        if (m_alertOthersOfSameType && m_mob->Level()) {
            // MC getNearbyMobsOfSameType: AABB.unitCubeFromLowerCorner(pos)
            // inflated by (followRange, 10, followRange).
            const double within = m_mob->GetAttributeValue(Attribute::FollowRange);
            AABB box;
            box.min = glm::vec3(m_mob->position);
            box.max = box.min + glm::vec3(1.0f);
            box.min -= glm::vec3(within, static_cast<float>(kAlertRangeY), within);
            box.max += glm::vec3(within, static_cast<float>(kAlertRangeY), within);

            std::vector<Entity*> nearby;
            m_mob->Level()->GetEntitiesInBox(box, m_mob, nearby);
            for (Entity* e : nearby) {
                if (e->GetType() != m_mob->GetType()) continue;
                if (auto* other = dynamic_cast<NeutralMob*>(e)) {
                    other->ForgetCurrentTargetAndRefreshUniversalAnger();
                }
            }
        }

        Goal::Start();
    }

} // namespace Game
