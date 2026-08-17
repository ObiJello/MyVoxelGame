// File: src/common/world/pathfinder/Path.hpp
//
// MC net.minecraft.world.level.pathfinder.Path — the result of a search: an
// ordered list of nodes plus a cursor the navigation advances.
//
// Nodes are stored BY VALUE here, unlike everywhere else in the pathfinder.
// The search's nodes live in the evaluator's arena and are freed the moment
// the call returns, but a path outlives it by many ticks — so this is the one
// place a copy is correct rather than wasteful.
#pragma once

#include "common/world/pathfinder/Node.hpp"

#include <glm/glm.hpp>
#include <vector>

namespace Game {

    class Entity;

    class Path {
    public:
        Path() = default;
        Path(std::vector<Node> nodes, const glm::ivec3& target, bool reached)
            : m_nodes(std::move(nodes)), m_target(target), m_reached(reached) {}

        bool IsDone() const { return m_nextNodeIndex >= static_cast<int>(m_nodes.size()); }
        bool IsEmpty() const { return m_nodes.empty(); }

        void Advance() { ++m_nextNodeIndex; }

        int  GetNodeCount() const { return static_cast<int>(m_nodes.size()); }
        int  GetNextNodeIndex() const { return m_nextNodeIndex; }
        void SetNextNodeIndex(int i) { m_nextNodeIndex = i; }

        const Node& GetNode(int i) const { return m_nodes[i]; }
        const Node& GetNextNode() const { return m_nodes[m_nextNodeIndex]; }
        glm::ivec3  GetNextNodePos() const {
            const Node& n = m_nodes[m_nextNodeIndex];
            return glm::ivec3(n.x, n.y, n.z);
        }

        const Node* GetEndNode() const { return m_nodes.empty() ? nullptr : &m_nodes.back(); }

        // MC Path.getNextEntityPos — the node centred on X/Z, offset so a wide
        // mob aims at a position its whole body can occupy rather than at the
        // node's corner.
        glm::dvec3 GetNextEntityPos(const Entity& entity) const;
        glm::dvec3 GetEntityPosAtNode(const Entity& entity, int index) const;

        bool CanReach() const { return m_reached; }
        const glm::ivec3& GetTarget() const { return m_target; }

        // Distance from the path's last node to the requested target. Used to
        // pick the least-bad partial path when the search failed.
        float GetDistToTarget() const;

        // MC Path.truncateNodes — used by GroundPathNavigation when a
        // sun-avoiding mob must stop at the first sky-lit node.
        void TruncateNodes(int count) {
            if (count < static_cast<int>(m_nodes.size())) m_nodes.resize(count);
        }

        bool SameAs(const Path& other) const;

    private:
        std::vector<Node> m_nodes;
        glm::ivec3        m_target{0};
        bool              m_reached = false;
        int               m_nextNodeIndex = 0;
    };

} // namespace Game
