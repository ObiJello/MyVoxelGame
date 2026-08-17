// File: src/common/world/pathfinder/PathFinder.hpp
//
// MC net.minecraft.world.level.pathfinder.PathFinder — the A*.
//
// Two properties of this search are deliberate and must survive any
// "optimisation":
//
//  * The heuristic is multiplied by FUDGING = 1.5, which makes it INADMISSIBLE.
//    The search is therefore greedy and does not guarantee an optimal path.
//    That is the point: it explores far fewer nodes and produces the slightly
//    impatient, beeline-ish routes MC mobs are known for. Dropping the 1.5
//    gives correct-but-wrong-looking paths and roughly triples the node count.
//
//  * When no target is reached, the search still returns the PARTIAL path to
//    whichever node got closest (each Target remembers its own best). A mob
//    that cannot reach you should still walk toward you and pile up against
//    the wall in between, not stand still.
#pragma once

#include "common/world/pathfinder/NodeEvaluator.hpp"
#include "common/world/pathfinder/Path.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace Game {

    class Mob;
    struct IBlockAccess;

    class PathFinder {
    public:
        PathFinder(std::unique_ptr<NodeEvaluator> evaluator, int maxVisitedNodes)
            : m_evaluator(std::move(evaluator)), m_maxVisitedNodes(maxVisitedNodes) {}

        static constexpr float kFudging = 1.5f;

        NodeEvaluator& GetNodeEvaluator() { return *m_evaluator; }

        void SetMaxVisitedNodes(int n) { m_maxVisitedNodes = n; }

        // `targets` are world positions to path to; the search stops as soon as
        // any is within `reachRange` (Manhattan) of an expanded node.
        std::optional<Path> FindPath(const IBlockAccess* blocks, Mob* mob,
                                     const std::vector<glm::ivec3>& targets,
                                     float maxPathLength, int reachRange,
                                     float maxVisitedNodesMultiplier);

    private:
        std::optional<Path> Search(Node* from, std::vector<Target>& targets,
                                   const std::vector<glm::ivec3>& targetPositions,
                                   float maxPathLength, int reachRange,
                                   float maxVisitedNodesMultiplier);

        float GetBestH(Node& from, std::vector<Target>& targets) const;
        static Path ReconstructPath(Node* closest, const glm::ivec3& target, bool reached);

        std::unique_ptr<NodeEvaluator> m_evaluator;
        int        m_maxVisitedNodes;
        BinaryHeap m_openSet;
        // MC sizes this at 32; the evaluator emits at most 8 (4 orthogonal +
        // 4 diagonal), but the headroom costs nothing and matches upstream.
        Node*      m_neighbors[32] = {};
    };

} // namespace Game
