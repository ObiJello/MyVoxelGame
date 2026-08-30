// File: src/common/entity/ai/goals/EndermanGoals.cpp
#include "common/entity/ai/goals/EndermanGoals.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/spawn/GeneratedSpawnTags.hpp"
#include "common/core/JavaRandom.hpp"

#include <cmath>
#include <vector>

namespace Game {

    // ── EndermanFreezeWhenLookedAt ─────────────────────────────────────────

    EndermanFreezeWhenLookedAt::EndermanFreezeWhenLookedAt(Enderman* enderman)
        : m_enderman(enderman) {
        SetFlags(GoalFlag::Jump | GoalFlag::Move);
    }

    bool EndermanFreezeWhenLookedAt::CanUse() {
        m_target = m_enderman->GetTarget();
        if (!m_target || !m_target->IsPlayer()) return false;
        // MC: only within 16 blocks — beyond that the stare rules are the
        // LookForPlayer goal's teleport business, not a freeze.
        if (m_target->DistanceToSqr(*m_enderman) > 256.0) return false;
        return m_enderman->IsBeingStaredBy(*m_target);
    }

    void EndermanFreezeWhenLookedAt::Start() {
        m_enderman->GetNavigation().Stop();
    }

    void EndermanFreezeWhenLookedAt::Tick() {
        if (!m_target) return;
        m_enderman->GetLookControl().SetLookAt(m_target->position.x, m_target->GetEyeY(),
                                               m_target->position.z);
    }

    void EndermanFreezeWhenLookedAt::ClearReferenceTo(const Entity* entity) {
        if (m_target == entity) m_target = nullptr;
    }

    // ── EndermanLookForPlayerGoal ──────────────────────────────────────────

    EndermanLookForPlayerGoal::EndermanLookForPlayerGoal(Enderman* enderman)
        : m_enderman(enderman) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Target));
    }

    bool EndermanLookForPlayerGoal::CanUse() {
        // MC EnderMan.java:390: getNearestPlayer with the isAngerInducing
        // selector BAKED IN — the nearest player MATCHING
        // isBeingStaredBy(player) || isAngryAt(player) wins, so a bystander
        // standing closer than the one staring does not mask the starer.
        // (The hasIndirectPassenger exemption is a rider check; a player
        // never rides an enderman.)
        EntityLevel* level = m_enderman->Level();
        if (!level) return false;

        const double range = m_enderman->GetAttributeValue(Attribute::FollowRange);
        std::vector<LivingEntity*> players;
        level->GetPlayers(players);

        LivingEntity* best = nullptr;
        double bestDistSq = 0.0;
        for (LivingEntity* p : players) {
            // The creative/spectator exemption is forCombat targeting's.
            if (p->IsCreative() || p->IsSpectator()) continue;
            const double d = m_enderman->DistanceToSqr(*p);
            if (d > range * range) continue;
            if (!m_enderman->IsBeingStaredBy(*p) && !m_enderman->IsAngryAt(*p)) continue;
            if (!best || d < bestDistSq) { best = p; bestDistSq = d; }
        }
        if (!best) return false;

        m_pendingTarget = best;
        return true;
    }

    void EndermanLookForPlayerGoal::Start() {
        // MC: a 5-tick fuse between the stare and the aggression.
        m_aggroTime = AdjustedTickDelay(5);
        m_teleportTime = 0;
    }

    void EndermanLookForPlayerGoal::Stop() {
        m_pendingTarget = nullptr;
        m_target = nullptr;
        m_enderman->SetTarget(nullptr);
    }

    bool EndermanLookForPlayerGoal::CanContinueToUse() {
        if (m_pendingTarget) {
            // MC re-tests the same isAngerInducing selector during the fuse.
            if (!m_enderman->IsBeingStaredBy(*m_pendingTarget) &&
                !m_enderman->IsAngryAt(*m_pendingTarget)) {
                return false;
            }
            // MC lookAt(pendingTarget, 10, 10) — locked on during the fuse.
            m_enderman->GetLookControl().SetLookAt(
                m_pendingTarget->position.x, m_pendingTarget->GetEyeY(),
                m_pendingTarget->position.z, 10.0f, 10.0f);
            return true;
        }
        if (m_target) {
            // MC continueAggroTargetConditions: forCombat().ignoreLineOfSight()
            // — keep the target while it lives and stays in follow range.
            if (!m_target->IsAlive()) return false;
            if (m_target->IsCreative() || m_target->IsSpectator()) return false;
            const double range = m_enderman->GetAttributeValue(Attribute::FollowRange);
            return m_enderman->DistanceToSqr(*m_target) <= range * range;
        }
        return false;
    }

    void EndermanLookForPlayerGoal::Tick() {
        if (m_enderman->GetTarget() == nullptr) m_target = nullptr;

        if (m_pendingTarget) {
            if (--m_aggroTime <= 0) {
                m_target = m_pendingTarget;
                m_pendingTarget = nullptr;
                m_enderman->SetTarget(m_target);
            }
            return;
        }

        if (m_target) {
            if (m_enderman->IsBeingStaredBy(*m_target)) {
                // MC: stared at from inside 4 blocks — blink away.
                if (m_target->DistanceToSqr(*m_enderman) < 16.0) {
                    m_enderman->Teleport();
                }
                m_teleportTime = 0;
            } else if (m_target->DistanceToSqr(*m_enderman) > 256.0 &&
                       m_teleportTime++ >= AdjustedTickDelay(30) &&
                       m_enderman->TeleportTowards(*m_target)) {
                m_teleportTime = 0;
            }
        }
    }

    void EndermanLookForPlayerGoal::ClearReferenceTo(const Entity* entity) {
        if (m_pendingTarget == entity) m_pendingTarget = nullptr;
        if (m_target == entity) m_target = nullptr;
    }

    // ── EndermanLeaveBlockGoal ─────────────────────────────────────────────

    bool EndermanLeaveBlockGoal::CanUse() {
        if (m_enderman->GetCarriedBlock() == BlockID::Air) return false;
        EntityLevel* level = m_enderman->Level();
        if (!level || !level->MobGriefing()) return false;
        return level->Random().NextInt(ReducedTickDelay(2000)) == 0;
    }

    void EndermanLeaveBlockGoal::Tick() {
        EntityLevel* level = m_enderman->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return;
        JavaRandom& rng = level->Random();

        const int xt = static_cast<int>(std::floor(
            m_enderman->position.x - 1.0 + rng.NextDouble() * 2.0));
        const int yt = static_cast<int>(std::floor(
            m_enderman->position.y + rng.NextDouble() * 2.0));
        const int zt = static_cast<int>(std::floor(
            m_enderman->position.z - 1.0 + rng.NextDouble() * 2.0));

        // MC canPlaceBlock: air here, a full solid (non-bedrock) below, no
        // entity in the cell. (canSurvive checks are the block's own — a
        // flower needs dirt; approximated by the full-cube-below rule.)
        if (blocks->GetBlock(xt, yt, zt) != BlockID::Air) return;
        const BlockID below = blocks->GetBlock(xt, yt - 1, zt);
        if (below == BlockID::Air || below == BlockID::Bedrock) return;
        if (!BlockRegistry::HasCollision(below)) return;

        std::vector<Entity*> occupants;
        AABB cell;
        cell.min = glm::vec3(xt, yt, zt);
        cell.max = glm::vec3(xt + 1, yt + 1, zt + 1);
        level->GetEntitiesInBox(cell, m_enderman, occupants);
        if (!occupants.empty()) return;

        level->SetBlock(glm::ivec3(xt, yt, zt), m_enderman->GetCarriedBlock());
        m_enderman->SetCarriedBlock(BlockID::Air);
    }

    // ── EndermanTakeBlockGoal ──────────────────────────────────────────────

    bool EndermanTakeBlockGoal::CanUse() {
        if (m_enderman->GetCarriedBlock() != BlockID::Air) return false;
        EntityLevel* level = m_enderman->Level();
        if (!level || !level->MobGriefing()) return false;
        return level->Random().NextInt(ReducedTickDelay(20)) == 0;
    }

    void EndermanTakeBlockGoal::Tick() {
        EntityLevel* level = m_enderman->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return;
        JavaRandom& rng = level->Random();

        const int xt = static_cast<int>(std::floor(
            m_enderman->position.x - 2.0 + rng.NextDouble() * 4.0));
        const int yt = static_cast<int>(std::floor(
            m_enderman->position.y + rng.NextDouble() * 3.0));
        const int zt = static_cast<int>(std::floor(
            m_enderman->position.z - 2.0 + rng.NextDouble() * 4.0));

        const BlockID block = blocks->GetBlock(xt, yt, zt);
        if (!SpawnTags::EndermanHoldable(block)) return;

        // MC clips a ray from the enderman to the block and requires it to be
        // the FIRST thing hit — no grabbing through walls. The same quarter-
        // block march the arrow and line-of-sight use.
        {
            const glm::dvec3 from(std::floor(m_enderman->position.x) + 0.5,
                                  yt + 0.5,
                                  std::floor(m_enderman->position.z) + 0.5);
            const glm::dvec3 to(xt + 0.5, yt + 0.5, zt + 0.5);
            const glm::dvec3 delta = to - from;
            const double dist = glm::length(delta);
            const int steps = std::max(1, static_cast<int>(std::ceil(dist / 0.25)));
            for (int i = 1; i < steps; ++i) {
                const glm::dvec3 p = from + delta * (static_cast<double>(i) / steps);
                const glm::ivec3 bp(static_cast<int>(std::floor(p.x)),
                                    static_cast<int>(std::floor(p.y)),
                                    static_cast<int>(std::floor(p.z)));
                if (bp.x == xt && bp.y == yt && bp.z == zt) break;
                if (BlockRegistry::HasCollision(blocks->GetBlock(bp.x, bp.y, bp.z))) {
                    return;   // something solid before the target
                }
            }
        }

        level->SetBlock(glm::ivec3(xt, yt, zt), BlockID::Air);
        m_enderman->SetCarriedBlock(block);
    }

} // namespace Game
