// File: src/common/entity/ai/navigation/WaterBoundPathNavigation.hpp
//
// MC WaterBoundPathNavigation + SwimNodeEvaluator.
//
// The swimmer's search space is water and nothing else: every cell the mob's
// box would occupy must be water fluid, or the node is BLOCKED. A cell of
// pathfindable air is BREACH — reachable only for the dolphin's leaping
// navigation (allowBreaching), and carrying WATER's default malus 8 so even a
// dolphin surfaces deliberately rather than incidentally. Leaving the fluid
// adds a flat +8 on top, which keeps fish OFF the surface.
#pragma once

#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"

#include <unordered_map>

namespace Game {

    class SwimNodeEvaluator : public NodeEvaluator {
    public:
        explicit SwimNodeEvaluator(bool allowBreaching)
            : m_allowBreaching(allowBreaching) {}

        void Prepare(const IBlockAccess* blocks, Mob* mob) override;
        void Done() override;

        Node* GetStart() override;
        int   GetNeighbors(Node** out, int maxOut, Node& from) override;
        PathType GetPathType(const PathfindingContext& ctx, int x, int y, int z) override;

    private:
        Node* SwimFindAcceptedNode(int x, int y, int z);
        PathType GetCachedBlockType(int x, int y, int z);
        static bool IsNodeValid(const Node* n) { return n && !n->closed; }
        static bool HasMalus(const Node* n) { return n && n->costMalus >= 0.0f; }

        bool m_allowBreaching;
        std::unordered_map<int64_t, PathType> m_pathTypeCache;
    };

    class WaterBoundPathNavigation : public PathNavigation {
    public:
        WaterBoundPathNavigation(Mob* mob, EntityLevel* level, bool allowBreaching = false)
            : PathNavigation(mob, level), m_allowBreaching(allowBreaching) {}

    protected:
        std::unique_ptr<PathFinder> CreatePathFinder(int maxVisitedNodes) override;
        bool CanUpdatePath() const override;
        glm::dvec3 GetTempMobPos() const override;
        double GetGroundY(const glm::dvec3& target) const override { return target.y; }
        bool CanMoveDirectly(const glm::dvec3& from, const glm::dvec3& to) const override;
        bool IsStableDestination(const glm::ivec3& pos) const override;

    private:
        bool m_allowBreaching;
    };

} // namespace Game
