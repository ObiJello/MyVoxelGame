// File: src/common/entity/ai/navigation/PathNavigation.hpp
//
// MC net.minecraft.world.entity.ai.navigation.{PathNavigation,
// GroundPathNavigation}.
//
// The navigation owns the path and advances a cursor along it; it is the ONLY
// thing that calls MoveControl::SetWantedPosition. Goals talk to the
// navigation, the navigation talks to the move control, the move control sets
// yaw and forward input, and LivingEntity::Travel turns that into motion. Each
// layer only knows about the next one.
//
// Two safety valves keep a mob from wedging itself forever, and both matter in
// a voxel world full of one-block ledges:
//   * the 100-tick stuck check — if the mob has covered less than a
//     speed-scaled threshold since the last check, the path is abandoned;
//   * the per-node timeout — if it has spent more than 3x the expected time
//     reaching one node, likewise.
#pragma once

#include "common/world/pathfinder/PathFinder.hpp"
#include "common/world/pathfinder/Path.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace Game {

    class Mob;
    class Entity;
    struct EntityLevel;

    class PathNavigation {
    public:
        PathNavigation(Mob* mob, EntityLevel* level);
        virtual ~PathNavigation() = default;

        static constexpr int   kMaxTimeRecompute = 20;
        static constexpr int   kStuckCheckInterval = 100;
        static constexpr float kStuckThresholdDistanceFactor = 0.25f;

        // ── Path creation ──────────────────────────────────────────────────
        std::optional<Path> CreatePath(const glm::ivec3& target, int reachRange);
        // MC's three-argument overload. `maxVisitedNodesMultiplier` scales the
        // node budget for ONE search without changing the navigation's own —
        // LongJumpToRandomPos passes 8, because it is asking "could I simply
        // walk there instead?" and a cheap negative would make the mob jump to
        // somewhere it could have strolled to.
        std::optional<Path> CreatePath(const glm::ivec3& target, int reachRange,
                                       float maxVisitedNodesMultiplier);
        std::optional<Path> CreatePath(const Entity& target, int reachRange);
        std::optional<Path> CreatePath(double x, double y, double z, int reachRange);

        // ── Path following ─────────────────────────────────────────────────
        bool MoveTo(double x, double y, double z, double speedModifier);
        bool MoveTo(const Entity& target, double speedModifier);
        bool MoveTo(std::optional<Path> path, double speedModifier);

        void Stop();
        bool IsDone() const { return !m_path.has_value() || m_path->IsDone(); }
        bool IsInProgress() const { return !IsDone(); }
        bool IsStuck() const { return m_isStuck; }

        const Path* GetPath() const { return m_path ? &(*m_path) : nullptr; }

        void SetSpeedModifier(double s) { m_speedModifier = s; }

        void SetCanFloat(bool v);
        bool CanFloat() const;

        // NOTE: there is deliberately no public GetNodeEvaluator(). The
        // evaluator does not exist until the first CreatePath, so any caller
        // reaching for it during mob construction gets a null dereference —
        // which is exactly the crash FloatGoal caused. Anything that needs an
        // evaluator flag should mirror it on the navigation, as canFloat does.

        void Tick();

        // MC PathNavigation.recomputePath — rate-limited to once per 20 ticks,
        // deferring the request when called sooner. Without the limit a mob
        // chasing a moving player re-runs A* every tick.
        void RecomputePath();


    protected:
        virtual std::unique_ptr<PathFinder> CreatePathFinder(int maxVisitedNodes) = 0;

        // MC canUpdatePath — a ground mob only follows its path while it has
        // footing; airborne it coasts and only advances the cursor when it has
        // fallen past the next node.
        virtual bool CanUpdatePath() const;

        // The position the navigation measures itself from. Ground navigation
        // overrides this to the surface Y so a mob standing on a slab does not
        // think it is a half block above its own path.
        virtual glm::dvec3 GetTempMobPos() const;

        virtual void TrimPath() {}

        // MC canCutCorner — some node types must be stepped on exactly rather
        // than clipped past. VIRTUAL because MC's frog overrides it to refuse
        // cutting a WATER_BORDER corner, which is what stops it clipping the
        // shoreline diagonally and landing back in the water.
        virtual bool CanCutCorner(PathType type) const;

        double GetMaxPathLength() const;
        // Virtual: amphibious navigation takes the target's own Y rather than
        // the ground beneath it, because "the ground" is meaningless in water.
        virtual double GetGroundY(const glm::dvec3& target) const;

        void FollowThePath();
        void DoStuckDetection(const glm::dvec3& mobPos);
        bool ShouldTargetNextNodeInDirection(const glm::dvec3& mobPos) const;
        virtual bool CanMoveDirectly(const glm::dvec3& from, const glm::dvec3& to) const { return false; }

        // MC isStableDestination — may the path END here. Ground navigation
        // wants solid footing; amphibious navigation accepts anything that is
        // not air below.
        virtual bool IsStableDestination(const glm::ivec3& pos) const;

        Mob*         m_mob;
        EntityLevel* m_level;

        std::unique_ptr<PathFinder> m_pathFinder;
        std::optional<Path>         m_path;

        double m_speedModifier = 0.0;
        int    m_tick = 0;
        int    m_lastStuckCheck = 0;
        glm::dvec3 m_lastStuckCheckPos{0.0};
        glm::dvec3 m_timeoutCachedNode{0.0};
        double m_timeoutLimit = 0.0;
        double m_timeoutTimer = 0.0;
        int64_t m_lastTimeoutCheck = 0;
        bool   m_isStuck = false;
        bool   m_hasDelayedRecomputation = false;
        // Mirrored here rather than living only on the node evaluator, which
        // does not exist until the first path is requested. See SetCanFloat.
        bool   m_canFloat = false;
        int64_t m_timeLastRecompute = 0;

        float m_maxDistanceToWaypoint = 0.5f;
        float m_maxVisitedNodesMultiplier = 1.0f;
        float m_requiredPathLength = 16.0f;
    };

    class GroundPathNavigation : public PathNavigation {
    public:
        GroundPathNavigation(Mob* mob, EntityLevel* level);

        void SetAvoidSun(bool v) { m_avoidSun = v; }

    protected:
        std::unique_ptr<PathFinder> CreatePathFinder(int maxVisitedNodes) override;
        bool CanUpdatePath() const override;
        glm::dvec3 GetTempMobPos() const override;
        void TrimPath() override;
        bool CanMoveDirectly(const glm::dvec3& from, const glm::dvec3& to) const override;

    private:
        int GetSurfaceY() const;

        bool m_avoidSun = false;
    };

} // namespace Game
