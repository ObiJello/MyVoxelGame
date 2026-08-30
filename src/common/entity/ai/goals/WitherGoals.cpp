// File: src/common/entity/ai/goals/WitherGoals.cpp
#include "common/entity/ai/goals/WitherGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/projectile/Projectile.hpp"

#include <vector>

namespace Game {

    // ── WitherDoNothingGoal ────────────────────────────────────────────────

    WitherDoNothingGoal::WitherDoNothingGoal(Wither* wither)
        : m_wither(wither) {
        SetFlags(GoalFlag::Move | GoalFlag::Jump | GoalFlag::Look);
    }

    bool WitherDoNothingGoal::CanUse() {
        return m_wither->GetInvulnerableTicks() > 0;
    }

    // ── WitherTargetGoal ───────────────────────────────────────────────────

    WitherTargetGoal::WitherTargetGoal(Wither* wither)
        : TargetGoal(wither, /*mustSee=*/false, /*mustReach=*/false),
          m_wither(wither) {
        m_conditions = TargetingConditions::ForCombat();
    }

    void WitherTargetGoal::FindTarget() {
        m_target = nullptr;
        EntityLevel* level = m_wither->Level();
        if (!level) return;

        const double follow = GetFollowDistance();
        m_conditions.range = follow;

        // MC LIVING_ENTITY_SELECTOR: not a WITHER_FRIEND (the #undead tag)
        // and attackable. Players first (they are not in the entity box
        // query), then every non-undead mob in the search area.
        LivingEntity* best = nullptr;
        double bestDistSq = 0.0;
        auto consider = [&](LivingEntity* living) {
            if (!living) return;
            if (IsUndeadEntityType(living->GetType())) return;
            // Projectiles ride the Mob pipeline here (MC's are not
            // LivingEntities and never reach this selector) — skip them.
            if (dynamic_cast<const Projectile*>(living)) return;
            if (!m_conditions.Test(m_wither, *living)) return;
            const double d = m_wither->DistanceToSqr(*living);
            if (!best || d < bestDistSq) { best = living; bestDistSq = d; }
        };

        std::vector<LivingEntity*> players;
        level->GetPlayers(players);
        for (LivingEntity* player : players) consider(player);

        AABB box = m_wither->GetAABB();
        box.min -= glm::vec3(follow, 4.0, follow);
        box.max += glm::vec3(follow, 4.0, follow);
        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_wither, nearby);
        for (Entity* e : nearby) consider(dynamic_cast<LivingEntity*>(e));

        m_target = best;
    }

    bool WitherTargetGoal::CanUse() {
        // MC passes randomInterval 0 — a full search on EVERY evaluation, no
        // 1-in-N gate.
        FindTarget();
        return m_target != nullptr;
    }

    void WitherTargetGoal::Start() {
        m_wither->SetTarget(m_target);
        m_targetMob = m_target;
        TargetGoal::Start();
    }

    void WitherTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_target == entity) m_target = nullptr;
    }

} // namespace Game
