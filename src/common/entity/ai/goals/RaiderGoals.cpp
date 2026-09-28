// File: src/common/entity/ai/goals/RaiderGoals.cpp
//
// See RaiderGoals.hpp. References (minecraft_code_26.3-pre-2/decompiled_net/
// minecraft/): world/entity/monster/PatrollingMonster.java
// (LongDistancePatrolGoal), world/entity/raid/Raider.java
// (HoldGroundAttackGoal).
#include "common/entity/ai/goals/RaiderGoals.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/raid/Raider.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>
#include <vector>

namespace Game {

    namespace {

        // MC Level.getHeightmapPos(MOTION_BLOCKING_NO_LEAVES, pos): the first
        // free cell above the column's highest motion-blocking (or fluid)
        // non-leaves block, by the heightmap's own predicate. The entity level
        // exposes no heightmap, so the column is walked from the top.
        int MotionBlockingNoLeavesY(const EntityLevel& level, int x, int z) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return level.GetMinY();
            for (int y = level.GetMaxY(); y >= level.GetMinY(); --y) {
                if (HeightmapIsOpaque(HeightmapType::MotionBlockingNoLeaves, blocks->GetBlock(x, y, z)) ||
                    blocks->IsBlockFluid(x, y, z)) {
                    return y + 1;
                }
            }
            return level.GetMinY();
        }

        // MC Vec3.yRot(angle): angle in radians, Mth.sin / Mth.cos.
        glm::dvec3 YRot(const glm::dvec3& v, float angle) {
            const double c = static_cast<double>(std::cos(angle));
            const double s = static_cast<double>(std::sin(angle));
            return glm::dvec3(v.x * c + v.z * s, v.y, v.z * c - v.x * s);
        }

        // MC Vec3.normalize: a vector shorter than 1e-5 normalises to zero.
        glm::dvec3 Normalize(const glm::dvec3& v) {
            const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return len < 1.0e-5 ? glm::dvec3(0.0) : v / len;
        }

        AABB Inflated(const AABB& box, float x, float y, float z) {
            return AABB::FromMinMax(box.min - glm::vec3(x, y, z), box.max + glm::vec3(x, y, z));
        }

        // MC level.getNearbyEntities(Raider.class, shoutTargeting, mob,
        // mob.getBoundingBox().inflate(8)) — shoutTargeting is non-combat,
        // range 8, line of sight and invisibility ignored.
        std::vector<Raider*> ShoutTargets(AbstractIllager& mob) {
            std::vector<Raider*> out;
            EntityLevel* level = mob.Level();
            if (!level) return out;
            const TargetingConditions shout =
                TargetingConditions::ForNonCombat().Range(8.0).IgnoreLineOfSight().IgnoreInvisibility();
            std::vector<Entity*> nearby;
            level->GetEntitiesInBox(Inflated(mob.GetAABB(), 8.0f, 8.0f, 8.0f), &mob, nearby);
            for (Entity* e : nearby) {
                auto* raider = dynamic_cast<Raider*>(e);
                if (raider && shout.Test(&mob, *raider)) out.push_back(raider);
            }
            return out;
        }

    } // namespace

    // ── LongDistancePatrolGoal ─────────────────────────────────────────────

    LongDistancePatrolGoal::LongDistancePatrolGoal(PatrollingMonster* mob, double speedModifier,
                                                   double leaderSpeedModifier)
        : m_mob(mob), m_speedModifier(speedModifier), m_leaderSpeedModifier(leaderSpeedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool LongDistancePatrolGoal::CanUse() {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        const bool isOnCooldown = level->GetGameTime() < m_cooldownUntil;
        return m_mob->IsPatrolling() && m_mob->GetTarget() == nullptr &&
               m_mob->GetControllingPassenger() == nullptr && m_mob->HasPatrolTarget() && !isOnCooldown;
    }

    void LongDistancePatrolGoal::Tick() {
        EntityLevel* level = m_mob->Level();
        if (!level) return;
        const bool patrolLeader = m_mob->IsPatrolLeader();
        PathNavigation& navigation = m_mob->GetNavigation();
        if (!navigation.IsDone()) return;

        // findPatrolCompanions: every PatrollingMonster within the box
        // inflated by 16 that may join a patrol, other than this one.
        std::vector<PatrollingMonster*> companions;
        {
            std::vector<Entity*> nearby;
            level->GetEntitiesInBox(Inflated(m_mob->GetAABB(), 16.0f, 16.0f, 16.0f), m_mob, nearby);
            for (Entity* e : nearby) {
                auto* other = dynamic_cast<PatrollingMonster*>(e);
                if (other && other != m_mob && other->CanJoinPatrol()) companions.push_back(other);
            }
        }

        if (m_mob->IsPatrolling() && companions.empty()) {
            m_mob->SetPatrolling(false);
            return;
        }

        const glm::ivec3 target = *m_mob->GetPatrolTarget();
        // BlockPos.closerToCenterThan(position, 10): the block centre within
        // 10 blocks (strictly).
        const glm::dvec3 centre(target.x + 0.5, target.y + 0.5, target.z + 0.5);
        const glm::dvec3 toCentre = centre - m_mob->position;
        if (patrolLeader && glm::dot(toCentre, toCentre) < 100.0) {
            m_mob->FindPatrolTarget();
            return;
        }

        glm::dvec3 longDistanceTarget(target.x + 0.5, target.y, target.z + 0.5);   // atBottomCenterOf
        const glm::dvec3 selfVector = m_mob->position;
        const glm::dvec3 distance = selfVector - longDistanceTarget;
        longDistanceTarget = YRot(distance, 90.0f) * 0.4 + longDistanceTarget;
        const glm::dvec3 moveTarget = Normalize(longDistanceTarget - selfVector) * 10.0 + selfVector;
        glm::ivec3 pathTarget(static_cast<int>(std::floor(moveTarget.x)),
                              static_cast<int>(std::floor(moveTarget.y)),
                              static_cast<int>(std::floor(moveTarget.z)));
        pathTarget.y = MotionBlockingNoLeavesY(*level, pathTarget.x, pathTarget.z);

        if (!navigation.MoveTo(pathTarget.x, pathTarget.y, pathTarget.z,
                               patrolLeader ? m_leaderSpeedModifier : m_speedModifier)) {
            MoveRandomly();
            m_cooldownUntil = level->GetGameTime() + kNavigationFailedCooldown;
        } else if (patrolLeader) {
            for (PatrollingMonster* companion : companions) companion->SetPatrolTarget(pathTarget);
        }
    }

    bool LongDistancePatrolGoal::MoveRandomly() {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        JavaRandom& random = level->Random();
        const glm::ivec3 pos = m_mob->BlockPosition();
        const int x = pos.x + (-8 + random.NextInt(16));
        const int z = pos.z + (-8 + random.NextInt(16));
        const int y = MotionBlockingNoLeavesY(*level, x, z);
        return m_mob->GetNavigation().MoveTo(x, y, z, m_speedModifier);
    }

    // ── HoldGroundAttackGoal ───────────────────────────────────────────────

    HoldGroundAttackGoal::HoldGroundAttackGoal(AbstractIllager* mob, float hostileRadius)
        : m_mob(mob), m_hostileRadiusSqr(hostileRadius * hostileRadius) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool HoldGroundAttackGoal::CanUse() {
        // getCurrentRaid() == null always (no raids).
        Entity* lastHurtByMob = m_mob->GetLastHurtByMob();
        return m_mob->IsPatrolling() && m_mob->GetTarget() != nullptr && !m_mob->IsAggressive() &&
               (lastHurtByMob == nullptr || !lastHurtByMob->IsPlayer());
    }

    void HoldGroundAttackGoal::Start() {
        m_mob->GetNavigation().Stop();
        LivingEntity* target = m_mob->GetTarget();
        for (Raider* raider : ShoutTargets(*m_mob)) raider->SetTarget(target);
    }

    void HoldGroundAttackGoal::Stop() {
        LivingEntity* target = m_mob->GetTarget();
        if (!target) return;
        for (Raider* raider : ShoutTargets(*m_mob)) {
            raider->SetTarget(target);
            raider->SetAggressive(true);
        }
        m_mob->SetAggressive(true);
    }

    void HoldGroundAttackGoal::Tick() {
        LivingEntity* target = m_mob->GetTarget();
        if (!target) return;
        if (m_mob->DistanceToSqr(*target) > static_cast<double>(m_hostileRadiusSqr)) {
            m_mob->GetLookControl().SetLookAt(target->position.x, target->GetEyeY(), target->position.z,
                                              30.0f, 30.0f);
            if (EntityLevel* level = m_mob->Level(); level && level->Random().NextInt(50) == 0) {
                m_mob->PlayAmbientSound();
            }
        } else {
            m_mob->SetAggressive(true);
        }
    }

} // namespace Game
