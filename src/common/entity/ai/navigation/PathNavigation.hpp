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
        // Virtual because MC's createPath(BlockPos, int) is: ground navigation
        // projects a mid-air or buried target onto a pathable surface first,
        // and the wall climber stashes the ultimate ask so its Tick can steer
        // straight at it when no path exists. All the other overloads funnel
        // through this one, exactly as MC's do.
        virtual std::optional<Path> CreatePath(const glm::ivec3& target, int reachRange);
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
        // Virtual for WallClimberNavigation, which falls back to steering
        // STRAIGHT at an unreachable target so the climb can begin.
        virtual bool MoveTo(const Entity& target, double speedModifier);
        bool MoveTo(std::optional<Path> path, double speedModifier);

        void Stop();
        // The mob moved to another level: path against its blocks from now
        // on. Drops the current path — it was through the old world.
        void SetLevel(EntityLevel* level) { m_level = level; Stop(); }
        bool IsDone() const { return !m_path.has_value() || m_path->IsDone(); }
        bool IsInProgress() const { return !IsDone(); }
        bool IsStuck() const { return m_isStuck; }

        const Path* GetPath() const { return m_path ? &(*m_path) : nullptr; }

        void SetSpeedModifier(double s) { m_speedModifier = s; }

        void SetCanFloat(bool v);
        bool CanFloat() const;

        // MC GroundPathNavigation.setCanOpenDoors — mirrored here like
        // canFloat (see SetCanFloat's lazy-evaluator note). Zombies with the
        // break-doors roll and vindicators are the setters.
        void SetCanOpenDoors(bool v);
        bool CanOpenDoors() const;

        // MC PathNavigation.setRequiredPathLength — a floor under the max path
        // length so a mob can path beyond its FOLLOW_RANGE (the copper golem
        // asks for 48 to cover its 32-block chest search; GetMaxPathLength
        // already takes the max). MC's updatePathfinderMaxVisitedNodes half is
        // skipped: the port sizes the node budget from the base follow range
        // once, at the lazy pathfinder creation.
        void SetRequiredPathLength(float length) { m_requiredPathLength = length; }

        // NOTE: there is deliberately no public GetNodeEvaluator(). The
        // evaluator does not exist until the first CreatePath, so any caller
        // reaching for it during mob construction gets a null dereference —
        // which is exactly the crash FloatGoal caused. Anything that needs an
        // evaluator flag should mirror it on the navigation, as canFloat does.

        // Virtual for FlyingPathNavigation, whose waypoint-advance rule
        // differs (block equality in three axes rather than the ground test).
        virtual void Tick();

        // MC PathNavigation.isStableDestination — PUBLIC because
        // GoalUtils.isNotStable (RandomPos) asks the mob's own navigation:
        // solid-below for a walker, any-non-air-below for an amphibian, any
        // non-solid cell for a swimmer. Routing wander targets through this is
        // what lets fish pick open water and fliers pick perches.
        virtual bool IsStableDestination(const glm::ivec3& pos) const;

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
        // MC PathNavigation.canMoveDirectly returns false; ONLY the flying,
        // water-bound and amphibious navigations override it. Ground mobs
        // never shortcut a waypoint — their sweep test knows nothing about
        // step height or fall damage.
        virtual bool CanMoveDirectly(const glm::dvec3& from, const glm::dvec3& to) const { return false; }

        // MC PathNavigation.timeoutPath / resetStuckTimeout. The reset clears
        // isStuck too — a fresh path (or an abandoned one) starts innocent, or
        // a mob once wedged reports IsStuck forever.
        void TimeoutPath();
        void ResetStuckTimeout();

        Mob*         m_mob;
        EntityLevel* m_level;

        std::unique_ptr<PathFinder> m_pathFinder;
        std::optional<Path>         m_path;

        // MC PathNavigation.targetPos/reachRange — what the last successful
        // CreatePath was ASKED for, so RecomputePath re-issues the same
        // request instead of re-deriving it from the path.
        std::optional<glm::ivec3>   m_targetPos;
        int                         m_reachRange = 0;

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
        bool   m_canOpenDoors = false;
        int64_t m_timeLastRecompute = 0;

        float m_maxDistanceToWaypoint = 0.5f;
        float m_maxVisitedNodesMultiplier = 1.0f;
        float m_requiredPathLength = 16.0f;
    };

    class GroundPathNavigation : public PathNavigation {
    public:
        GroundPathNavigation(Mob* mob, EntityLevel* level);

        // The override hides the base's other CreatePath overloads without this.
        using PathNavigation::CreatePath;

        // MC GroundPathNavigation.createPath(BlockPos, int) — null when the
        // target's chunk is not loaded, and (unless the below-surface toggle
        // is set) the target is first projected onto a pathable surface.
        std::optional<Path> CreatePath(const glm::ivec3& target, int reachRange) override;

        void SetAvoidSun(bool v) { m_avoidSun = v; }
        // MC GroundPathNavigation.setCanPathToTargetsBelowSurface — flipped by
        // the copper golem's transport behaviour so a chest sunk into the
        // floor stays a legal target.
        void SetCanPathToTargetsBelowSurface(bool v) { m_canPathToTargetsBelowSurface = v; }

    protected:
        std::unique_ptr<PathFinder> CreatePathFinder(int maxVisitedNodes) override;
        bool CanUpdatePath() const override;
        glm::dvec3 GetTempMobPos() const override;
        void TrimPath() override;

    private:
        int GetSurfaceY() const;
        // MC GroundPathNavigation.findSurfacePosition — walk a mid-air target
        // down to the ground (or up out of a cave ceiling / solid column).
        glm::ivec3 FindSurfacePosition(const glm::ivec3& target) const;

        bool m_avoidSun = false;
        bool m_canPathToTargetsBelowSurface = false;
    };

    // MC WallClimberNavigation — the spider's. A ground navigation that
    // remembers the position it was ULTIMATELY asked for: when the A* cannot
    // produce a path (the target is up a wall), it steers straight at the
    // remembered position instead, and the spider's climb-on-collision does
    // the rest.
    class WallClimberNavigation : public GroundPathNavigation {
    public:
        WallClimberNavigation(Mob* mob, EntityLevel* level)
            : GroundPathNavigation(mob, level) {}

        using GroundPathNavigation::CreatePath;

        // MC WallClimberNavigation.createPath — EVERY path request stashes the
        // ultimate ask, even one that succeeds, so once the path runs out the
        // spider keeps steering at the remembered position until it is within
        // its own body width of the column.
        std::optional<Path> CreatePath(const glm::ivec3& target, int reachRange) override;
        bool MoveTo(const Entity& target, double speedModifier) override;
        void Tick() override;

    private:
        std::optional<glm::ivec3> m_pathToPosition;
    };

} // namespace Game
