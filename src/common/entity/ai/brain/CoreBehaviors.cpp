// File: src/common/entity/ai/brain/CoreBehaviors.cpp
#include "common/entity/ai/brain/CoreBehaviors.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ── LookAtTargetSink ───────────────────────────────────────────────────

    LookAtTargetSink::LookAtTargetSink(int minDuration, int maxDuration)
        : Behavior({ MemoryCondition{ MemoryModule::LookTarget,
                                      MemoryStatus::ValuePresent } },
                   minDuration, maxDuration) {}

    bool LookAtTargetSink::CanStillUse(EntityLevel&, LivingEntity& body, int64_t) {
        const Brain* brain = body.GetBrain();
        if (!brain) return false;
        const PositionTracker* t = brain->GetPositionTracker(MemoryModule::LookTarget);
        return t != nullptr && t->IsVisibleBy(body);
    }

    void LookAtTargetSink::Tick(EntityLevel&, LivingEntity& body, int64_t) {
        Brain* brain = body.GetBrain();
        auto* mob = dynamic_cast<Mob*>(&body);
        if (!brain || !mob) return;
        if (const PositionTracker* t = brain->GetPositionTracker(MemoryModule::LookTarget)) {
            mob->GetLookControl().SetLookAt(t->CurrentPosition());
        }
    }

    void LookAtTargetSink::Stop(EntityLevel&, LivingEntity& body, int64_t) {
        if (Brain* brain = body.GetBrain()) brain->EraseMemory(MemoryModule::LookTarget);
    }

    // ── MoveToTargetSink ───────────────────────────────────────────────────

    MoveToTargetSink::MoveToTargetSink(int minTimeout, int maxTimeout)
        : Behavior({ MemoryCondition{ MemoryModule::CantReachWalkTargetSince,
                                      MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::Path, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::WalkTarget,
                                      MemoryStatus::ValuePresent } },
                   minTimeout, maxTimeout) {}

    bool MoveToTargetSink::ReachedTarget(const Mob& body, const WalkTarget& target) {
        // MC uses MANHATTAN distance here, not euclidean. With closeEnoughDist
        // 1 that is a plus-shaped acceptance region rather than a circle, and
        // using euclidean instead makes mobs stop a block short on diagonals.
        const glm::ivec3 t = target.target.CurrentBlockPosition();
        const glm::ivec3 p = body.BlockPosition();
        const int manhattan = std::abs(t.x - p.x) + std::abs(t.y - p.y) + std::abs(t.z - p.z);
        return manhattan <= target.closeEnoughDist;
    }

    bool MoveToTargetSink::TryComputePath(Mob& body, const WalkTarget& target,
                                          int64_t timestamp) {
        const glm::ivec3 targetPos = target.target.CurrentBlockPosition();
        m_path = body.GetNavigation().CreatePath(targetPos, 0);
        m_speedModifier = target.speedModifier;

        Brain* brain = body.GetBrain();
        if (!brain) return false;

        if (ReachedTarget(body, target)) {
            brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
            return m_path.has_value();
        }

        const bool canReach = m_path.has_value() && m_path->CanReach();
        if (canReach) {
            brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
        } else if (!brain->HasMemoryValue(MemoryModule::CantReachWalkTargetSince)) {
            // MC records WHEN the mob first failed to reach, and behaviours read
            // the age of that memory to give up. A boolean would lose that.
            brain->SetMemory(MemoryModule::CantReachWalkTargetSince, timestamp);
        }
        // MC falls back to a partial step toward the target when no full path
        // exists. DefaultRandomPos::GetPosTowards is the same helper the goal
        // system's stroll uses.
        return m_path.has_value();
    }

    bool MoveToTargetSink::CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) {
        if (m_remainingCooldown > 0) {
            --m_remainingCooldown;
            return false;
        }
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        const WalkTarget* target = brain->GetWalkTarget(MemoryModule::WalkTarget);
        if (!target) return false;
        const WalkTarget copy = *target;   // TryComputePath may erase the memory

        const bool reached = ReachedTarget(*mob, copy);
        if (!reached && TryComputePath(*mob, copy, level.GetGameTime())) {
            m_lastTargetPos = copy.target.CurrentBlockPosition();
            return true;
        }

        brain->EraseMemory(MemoryModule::WalkTarget);
        if (reached) brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
        return false;
    }

    bool MoveToTargetSink::CanStillUse(EntityLevel&, LivingEntity& body, int64_t) {
        if (!m_path.has_value() || !m_lastTargetPos.has_value()) return false;
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        const WalkTarget* target = brain->GetWalkTarget(MemoryModule::WalkTarget);
        return !mob->GetNavigation().IsDone()
            && target != nullptr
            && !ReachedTarget(*mob, *target);
    }

    void MoveToTargetSink::Start(EntityLevel&, LivingEntity& body, int64_t) {
        auto* mob = dynamic_cast<Mob*>(&body);
        if (!mob) return;
        mob->GetNavigation().MoveTo(m_path, static_cast<double>(m_speedModifier));
    }

    void MoveToTargetSink::Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain || !m_lastTargetPos.has_value()) return;

        const WalkTarget* target = brain->GetWalkTarget(MemoryModule::WalkTarget);
        if (!target) return;

        // MC re-paths when the target has MOVED more than 2 blocks (distSqr > 4)
        // — which is what keeps a mob following a walking player instead of
        // walking to where the player used to be.
        const glm::ivec3 now = target->target.CurrentBlockPosition();
        const glm::ivec3 d = now - *m_lastTargetPos;
        if (static_cast<double>(d.x * d.x + d.y * d.y + d.z * d.z) > 4.0) {
            const WalkTarget copy = *target;
            if (TryComputePath(*mob, copy, level.GetGameTime())) {
                m_lastTargetPos = now;
                Start(level, body, timestamp);
            }
        }
    }

    void MoveToTargetSink::Stop(EntityLevel& level, LivingEntity& body, int64_t) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (mob && brain) {
            const WalkTarget* target = brain->GetWalkTarget(MemoryModule::WalkTarget);
            if (target && !ReachedTarget(*mob, *target) && mob->GetNavigation().IsStuck()) {
                m_remainingCooldown = level.Random().NextInt(40);
            }
            mob->GetNavigation().Stop();
            brain->EraseMemory(MemoryModule::WalkTarget);
            brain->EraseMemory(MemoryModule::Path);
        }
        m_path.reset();
    }

    // ── NearestLivingEntitySensor ──────────────────────────────────────────

    void NearestLivingEntitySensor::DoTick(EntityLevel& level, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;

        // MC scans a box of the mob's FOLLOW_RANGE horizontally and half that
        // vertically, then sorts by distance — the sort is what makes every
        // "nearest X" query a scan-until-first-match.
        const double range = body.GetAttributeValue(Attribute::FollowRange);
        AABB box = body.GetAABB();
        box.min -= glm::vec3(range, range * 0.5, range);
        box.max += glm::vec3(range, range * 0.5, range);

        std::vector<Entity*> found;
        level.GetEntitiesInBox(box, &body, found);

        std::vector<Entity*> living;
        living.reserve(found.size());
        for (Entity* e : found) {
            if (auto* l = dynamic_cast<LivingEntity*>(e)) {
                if (l->IsAlive()) living.push_back(l);
            }
        }
        std::sort(living.begin(), living.end(), [&](Entity* a, Entity* b) {
            return body.DistanceToSqr(*a) < body.DistanceToSqr(*b);
        });

        brain->SetMemory(MemoryModule::NearestLivingEntities, living);

        // The visible subset. MC applies the mob's own line-of-sight test, so a
        // mob does not react to something through a wall.
        NearestVisibleLivingEntities visible;
        auto* mob = dynamic_cast<Mob*>(&body);
        for (Entity* e : living) {
            auto* l = static_cast<LivingEntity*>(e);
            if (!mob || mob->GetSensing().HasLineOfSight(*l)) visible.entities.push_back(l);
        }
        brain->SetMemory(MemoryModule::NearestVisibleLivingEntities, std::move(visible));
    }

} // namespace Game
