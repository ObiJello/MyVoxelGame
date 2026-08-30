// File: src/common/entity/ai/navigation/FlyingPathNavigation.hpp
//
// MC FlyingPathNavigation + FlyNodeEvaluator.
//
// A flyer's search space is the full 26-neighbourhood: every horizontal,
// vertical and diagonal step, each diagonal gated on ALL the orthogonal nodes
// it clips past being passable — the same no-corner-cutting rule the walker
// applies in 2D, lifted to 3D. There is no floor logic at all; instead OPEN
// air above something walkable is folded down to that surface's type so a
// flyer still prefers to route over safe ground (WALKABLE nodes carry a +1
// malus, making pure flight marginally cheaper than hugging terrain).
#pragma once

#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"

#include <unordered_map>

namespace Game {

    class FlyNodeEvaluator : public WalkNodeEvaluator {
    public:
        void Prepare(const IBlockAccess* blocks, Mob* mob) override;
        void Done() override;

        Node* GetStart() override;
        int   GetNeighbors(Node** out, int maxOut, Node& from) override;
        PathType GetPathType(const PathfindingContext& ctx, int x, int y, int z) override;

    private:
        Node* FlyFindAcceptedNode(int x, int y, int z);
        PathType GetCachedFlyPathType(int x, int y, int z);
        static bool HasMalus(const Node* node) { return node && node->costMalus >= 0.0f; }
        static bool IsOpenNode(const Node* node) { return node && !node->closed; }

        std::unordered_map<int64_t, PathType> m_flyPathTypeCache;
    };

    class FlyingPathNavigation : public PathNavigation {
    public:
        FlyingPathNavigation(Mob* mob, EntityLevel* level)
            : PathNavigation(mob, level) {}

        // MC FlyingPathNavigation.tick: the flyer advances waypoints by block
        // EQUALITY when it cannot repath (in liquid without canFloat), and
        // hands the move control the next position in all three axes.
        void Tick() override;

        // MC FlyingPathNavigation.isStableDestination — entityCanStandOn: the
        // block AT the position must offer a sturdy top face to perch on,
        // unlike the walker's solid-BELOW test.
        bool IsStableDestination(const glm::ivec3& pos) const override;

    protected:
        std::unique_ptr<PathFinder> CreatePathFinder(int maxVisitedNodes) override;
        bool CanUpdatePath() const override;
        glm::dvec3 GetTempMobPos() const override;
        double GetGroundY(const glm::dvec3& target) const override { return target.y; }
        bool CanMoveDirectly(const glm::dvec3& from, const glm::dvec3& to) const override;
    };

} // namespace Game
