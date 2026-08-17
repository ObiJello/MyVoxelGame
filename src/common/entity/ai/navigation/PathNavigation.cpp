// File: src/common/entity/ai/navigation/PathNavigation.cpp
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    PathNavigation::PathNavigation(Mob* mob, EntityLevel* level)
        : m_mob(mob), m_level(level) {}

    void PathNavigation::SetCanFloat(bool v) {
        // Stored on the navigation, NOT forwarded straight to the evaluator.
        //
        // The pathfinder (and therefore the evaluator) is created lazily on the
        // first CreatePath, because its node budget depends on the mob's final
        // FOLLOW_RANGE — which a concrete mob only sets AFTER Mob's constructor
        // has already built the navigation. But FloatGoal's constructor calls
        // this during RegisterGoals, long before any path exists, so reaching
        // through m_pathFinder here dereferences null.
        m_canFloat = v;
        if (m_pathFinder) m_pathFinder->GetNodeEvaluator().SetCanFloat(v);
    }

    bool PathNavigation::CanFloat() const {
        // Same reason as above: GetSurfaceY asks this every tick, including on
        // ticks where the mob has no path.
        return m_canFloat;
    }

    double PathNavigation::GetMaxPathLength() const {
        // MC: max(FOLLOW_RANGE, requiredPathLength). The follow range alone
        // would cap a 16-range mob's paths at 16 blocks, which is shorter than
        // many of the detours it legitimately needs.
        return std::max(m_mob->GetAttributeValue(Attribute::FollowRange),
                        static_cast<double>(m_requiredPathLength));
    }

    bool PathNavigation::CanUpdatePath() const {
        return m_mob->onGround || m_mob->IsInLiquid();
    }

    glm::dvec3 PathNavigation::GetTempMobPos() const {
        return m_mob->position;
    }

    bool PathNavigation::CanCutCorner(PathType type) const {
        return type != PathType::DangerFire && type != PathType::DangerOther &&
               type != PathType::WalkableDoor;
    }

    bool PathNavigation::IsStableDestination(const glm::ivec3& pos) const {
        // MC's default: the block below must be solid enough to stand on.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        return blocks && blocks->IsBlockSolid(pos.x, pos.y - 1, pos.z);
    }

    double PathNavigation::GetGroundY(const glm::dvec3& target) const {
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return target.y;

        const glm::ivec3 pos(static_cast<int>(std::floor(target.x)),
                             static_cast<int>(std::floor(target.y)),
                             static_cast<int>(std::floor(target.z)));

        // Air below: aim at the node's own Y (the mob is going to fall).
        // Otherwise aim at the actual top surface, so a slab or a stair does
        // not leave the mob perpetually trying to walk into its own floor.
        if (blocks->GetBlock(pos.x, pos.y - 1, pos.z) == BlockID::Air) return target.y;
        return WalkNodeEvaluator::GetFloorLevel(*blocks, pos);
    }

    std::optional<Path> PathNavigation::CreatePath(double x, double y, double z, int reachRange) {
        return CreatePath(glm::ivec3(static_cast<int>(std::floor(x)),
                                     static_cast<int>(std::floor(y)),
                                     static_cast<int>(std::floor(z))),
                          reachRange);
    }

    std::optional<Path> PathNavigation::CreatePath(const glm::ivec3& target, int reachRange) {
        PROFILE_ZONE_N("Nav.CreatePath");

        if (!m_pathFinder) {
            // MC sizes the node budget from the mob's BASE follow range, not
            // its modified one, so a mob with a spawn bonus does not get a
            // bigger search than its species.
            const int maxVisited = static_cast<int>(
                std::floor(m_mob->Attributes().GetBaseValue(Attribute::FollowRange) * 16.0));
            m_pathFinder = CreatePathFinder(std::max(1, maxVisited));

            // Replay the flags that were set before the evaluator existed —
            // SetCanFloat is called from FloatGoal's constructor, which runs
            // during mob construction. Without this the flag is silently lost
            // and a floating mob refuses to path across water.
            m_pathFinder->GetNodeEvaluator().SetCanFloat(m_canFloat);
        }

        // Already heading there and the path is still live — reuse it rather
        // than paying for an identical search.
        if (m_path && !m_path->IsDone() && m_path->GetTarget() == target) {
            return m_path;
        }

        std::vector<glm::ivec3> targets{target};
        std::optional<Path> path = m_pathFinder->FindPath(
            m_level->Blocks(), m_mob, targets,
            static_cast<float>(GetMaxPathLength()), reachRange,
            m_maxVisitedNodesMultiplier);

        return path;
    }

    std::optional<Path> PathNavigation::CreatePath(const glm::ivec3& target, int reachRange,
                                                  float maxVisitedNodesMultiplier) {
        const float saved = m_maxVisitedNodesMultiplier;
        m_maxVisitedNodesMultiplier = maxVisitedNodesMultiplier;
        std::optional<Path> path = CreatePath(target, reachRange);
        m_maxVisitedNodesMultiplier = saved;
        return path;
    }

    std::optional<Path> PathNavigation::CreatePath(const Entity& target, int reachRange) {
        return CreatePath(target.BlockPosition(), reachRange);
    }

    bool PathNavigation::MoveTo(double x, double y, double z, double speedModifier) {
        return MoveTo(CreatePath(x, y, z, 1), speedModifier);
    }

    bool PathNavigation::MoveTo(const Entity& target, double speedModifier) {
        return MoveTo(CreatePath(target, 1), speedModifier);
    }

    bool PathNavigation::MoveTo(std::optional<Path> path, double speedModifier) {
        if (!path || path->IsEmpty()) {
            m_path.reset();
            return false;
        }

        // Reusing the identical path keeps the cursor where it is; replacing it
        // would restart the mob at node 0 and make it walk backwards.
        if (!m_path || !m_path->SameAs(*path)) {
            m_path = std::move(path);
        }

        TrimPath();
        if (m_path->GetNodeCount() <= 0) return false;

        m_speedModifier = speedModifier;
        const glm::dvec3 mobPos = GetTempMobPos();
        m_lastStuckCheck = m_tick;
        m_lastStuckCheckPos = mobPos;
        return true;
    }

    void PathNavigation::Stop() {
        m_path.reset();
    }

    void PathNavigation::RecomputePath() {
        if (!m_level) return;
        const int64_t now = m_level->GetGameTime();
        if (now - m_timeLastRecompute > kMaxTimeRecompute) {
            if (m_path) {
                const glm::ivec3 target = m_path->GetTarget();
                m_path.reset();
                m_path = CreatePath(target, 1);
            }
            m_timeLastRecompute = now;
            m_hasDelayedRecomputation = false;
        } else {
            m_hasDelayedRecomputation = true;
        }
    }

    void PathNavigation::Tick() {
        ++m_tick;

        if (m_hasDelayedRecomputation) RecomputePath();
        if (IsDone()) return;

        if (CanUpdatePath()) {
            FollowThePath();
        } else if (m_path && !m_path->IsDone()) {
            // Airborne. MC still advances the cursor when the mob has fallen
            // past the next node in the same column — otherwise a mob that
            // jumped a gap would keep steering at a node beneath its feet.
            const glm::dvec3 mobPos = GetTempMobPos();
            const glm::dvec3 nodePos = m_path->GetNextEntityPos(*m_mob);
            if (mobPos.y > nodePos.y && !m_mob->onGround &&
                std::floor(mobPos.x) == std::floor(nodePos.x) &&
                std::floor(mobPos.z) == std::floor(nodePos.z)) {
                m_path->Advance();
            }
        }

        if (IsDone()) return;

        const glm::dvec3 target = m_path->GetNextEntityPos(*m_mob);
        m_mob->GetMoveControl().SetWantedPosition(target.x, GetGroundY(target), target.z,
                                                  m_speedModifier);
    }

    void PathNavigation::FollowThePath() {
        const glm::dvec3 mobPos = GetTempMobPos();

        // MC narrows the acceptance radius for WIDE mobs and widens it for
        // narrow ones, so a spider does not orbit a waypoint it is already
        // standing on and a chicken still has to actually arrive.
        const float width = m_mob->GetBbWidth();
        m_maxDistanceToWaypoint = width > 0.75f ? width / 2.0f : 0.75f - width / 2.0f;

        const glm::ivec3 node = m_path->GetNextNodePos();
        const double xDist = std::abs(m_mob->position.x - (static_cast<double>(node.x) + 0.5));
        const double yDist = std::abs(m_mob->position.y -  static_cast<double>(node.y));
        const double zDist = std::abs(m_mob->position.z - (static_cast<double>(node.z) + 0.5));

        const bool closeEnough = xDist < m_maxDistanceToWaypoint &&
                                 zDist < m_maxDistanceToWaypoint &&
                                 yDist < 1.0;

        if (closeEnough ||
            (CanCutCorner(m_path->GetNextNode().type) && ShouldTargetNextNodeInDirection(mobPos))) {
            m_path->Advance();
        }

        DoStuckDetection(mobPos);
    }

    bool PathNavigation::ShouldTargetNextNodeInDirection(const glm::dvec3& mobPos) const {
        const int nextIndex = m_path->GetNextNodeIndex() + 1;
        if (nextIndex >= m_path->GetNodeCount()) return false;

        const glm::ivec3 cur = m_path->GetNextNodePos();
        const glm::dvec3 currentNode(cur.x + 0.5, cur.y, cur.z + 0.5);

        const glm::dvec3 dCur = currentNode - mobPos;
        if (dCur.x * dCur.x + dCur.y * dCur.y + dCur.z * dCur.z > 4.0) return false;

        if (CanMoveDirectly(mobPos, m_path->GetNextEntityPos(*m_mob))) return true;

        const Node& n = m_path->GetNode(nextIndex);
        const glm::dvec3 nextNode(n.x + 0.5, n.y, n.z + 0.5);
        const glm::dvec3 dNext = nextNode - mobPos;

        const double curSq  = dCur.x * dCur.x + dCur.y * dCur.y + dCur.z * dCur.z;
        const double nextSq = dNext.x * dNext.x + dNext.y * dNext.y + dNext.z * dNext.z;

        const bool closerToNext = nextSq < curSq;
        const bool withinCurrentBlock = curSq < 0.5;
        if (!closerToNext && !withinCurrentBlock) return false;

        // Only skip ahead when the two nodes are on OPPOSITE sides of the mob:
        // it has overshot the current one, and steering back would zigzag.
        const glm::dvec3 dirCur = glm::normalize(dCur);
        const glm::dvec3 dirNext = glm::normalize(dNext);
        return glm::dot(dirNext, dirCur) < 0.0;
    }

    void PathNavigation::DoStuckDetection(const glm::dvec3& mobPos) {
        if (m_tick - m_lastStuckCheck > kStuckCheckInterval) {
            const float speed = m_mob->GetSpeed();
            // Speeds below 1 are squared, which makes the threshold fall off
            // fast for slow mobs so they are not declared stuck for dawdling.
            const float effectiveSpeed = speed >= 1.0f ? speed : speed * speed;
            const float threshold = effectiveSpeed * kStuckCheckInterval * kStuckThresholdDistanceFactor;

            const glm::dvec3 d = mobPos - m_lastStuckCheckPos;
            if (d.x * d.x + d.y * d.y + d.z * d.z < static_cast<double>(threshold) * threshold) {
                m_isStuck = true;
                Stop();
            } else {
                m_isStuck = false;
            }

            m_lastStuckCheck = m_tick;
            m_lastStuckCheckPos = mobPos;
        }

        if (!m_path || m_path->IsDone()) return;

        const glm::ivec3 nodePos = m_path->GetNextNodePos();
        const int64_t now = m_level->GetGameTime();
        const glm::dvec3 nodeCentre(nodePos.x + 0.5, nodePos.y, nodePos.z + 0.5);

        if (nodeCentre == m_timeoutCachedNode) {
            m_timeoutTimer += static_cast<double>(now - m_lastTimeoutCheck);
        } else {
            m_timeoutCachedNode = nodeCentre;
            const glm::dvec3 d = nodeCentre - mobPos;
            const double dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            m_timeoutLimit = m_mob->GetSpeed() > 0.0f
                ? dist / static_cast<double>(m_mob->GetSpeed()) * 20.0
                : 0.0;
            m_timeoutTimer = 0.0;
        }

        if (m_timeoutLimit > 0.0 && m_timeoutTimer > m_timeoutLimit * 3.0) {
            m_timeoutCachedNode = glm::dvec3(0.0);
            m_timeoutTimer = 0.0;
            m_timeoutLimit = 0.0;
            Stop();
        }

        m_lastTimeoutCheck = now;
    }

    // ── GroundPathNavigation ───────────────────────────────────────────────

    GroundPathNavigation::GroundPathNavigation(Mob* mob, EntityLevel* level)
        : PathNavigation(mob, level) {}

    std::unique_ptr<PathFinder> GroundPathNavigation::CreatePathFinder(int maxVisitedNodes) {
        auto evaluator = std::make_unique<WalkNodeEvaluator>();
        evaluator->SetCanPassDoors(true);
        return std::make_unique<PathFinder>(std::move(evaluator), maxVisitedNodes);
    }

    bool GroundPathNavigation::CanUpdatePath() const {
        return m_mob->onGround || m_mob->IsInLiquid();
    }

    int GroundPathNavigation::GetSurfaceY() const {
        // A floating mob standing in water measures from the SURFACE, not from
        // the bottom, so its path nodes line up with where it actually is.
        if (!m_mob->IsInWater() || !CanFloat()) {
            return static_cast<int>(std::floor(m_mob->position.y + 0.5));
        }

        const IBlockAccess* blocks = m_level->Blocks();
        int y = static_cast<int>(std::floor(m_mob->position.y));
        const int x = static_cast<int>(std::floor(m_mob->position.x));
        const int z = static_cast<int>(std::floor(m_mob->position.z));

        // MC caps the walk-up at 16 so a mob at the bottom of an ocean does not
        // scan the whole column.
        for (int i = 0; i < 16 && blocks && blocks->IsBlockFluid(x, y, z); ++i) {
            ++y;
        }
        return y;
    }

    glm::dvec3 GroundPathNavigation::GetTempMobPos() const {
        return glm::dvec3(m_mob->position.x, static_cast<double>(GetSurfaceY()), m_mob->position.z);
    }

    bool GroundPathNavigation::CanMoveDirectly(const glm::dvec3& from, const glm::dvec3& to) const {
        // MC does a swept box test here. Reusing CollidesAt at a few samples
        // along the segment is the same question asked more cheaply, and the
        // only consequence of a false negative is that the mob follows its path
        // node-by-node instead of cutting the corner.
        if (!m_level->Blocks()) return false;

        PhysicsContext ctx = m_level->Physics();
        const glm::vec3 half = m_mob->HalfExtents();
        const glm::dvec3 delta = to - from;
        const double dist = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        const int steps = std::max(1, static_cast<int>(std::ceil(dist * 2.0)));

        for (int i = 1; i <= steps; ++i) {
            const glm::dvec3 p = from + delta * (static_cast<double>(i) / steps);
            const AABB box(glm::vec3(p.x, p.y + half.y, p.z), half * 2.0f);
            if (CollidesAt(box, ctx)) return false;
        }
        return true;
    }

    void GroundPathNavigation::TrimPath() {
        if (!m_avoidSun || !m_path || !m_level) return;

        // Skeletons stop at the first sky-lit node so they never path out of
        // the shade into their own death.
        for (int i = 0; i < m_path->GetNodeCount(); ++i) {
            const Node& n = m_path->GetNode(i);
            if (m_level->CanSeeSky(n.x, n.y, n.z)) {
                m_path->TruncateNodes(i);
                return;
            }
        }
    }

} // namespace Game
