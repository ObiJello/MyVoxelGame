// File: src/common/world/pathfinder/Path.cpp
#include "common/world/pathfinder/Path.hpp"
#include "common/entity/Entity.hpp"

#include <cmath>

namespace Game {

    glm::dvec3 Path::GetEntityPosAtNode(const Entity& entity, int index) const {
        const Node& n = m_nodes[index];

        // MC Path.getEntityPosAtNode: centre the mob on the node's X/Z. The
        // 0.5 offsets are scaled by width so a 1.4-wide spider aims at a spot
        // its body actually fits, rather than at the node corner.
        const double halfWidth = std::floor(entity.GetBbWidth() + 1.0) * 0.5;
        return glm::dvec3(static_cast<double>(n.x) + halfWidth,
                          static_cast<double>(n.y),
                          static_cast<double>(n.z) + halfWidth);
    }

    glm::dvec3 Path::GetNextEntityPos(const Entity& entity) const {
        return GetEntityPosAtNode(entity, m_nextNodeIndex);
    }

    float Path::GetDistToTarget() const {
        const Node* end = GetEndNode();
        if (!end) return 3.4028235e38f;

        const float dx = static_cast<float>(m_target.x - end->x);
        const float dy = static_cast<float>(m_target.y - end->y);
        const float dz = static_cast<float>(m_target.z - end->z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    bool Path::SameAs(const Path& other) const {
        if (m_nodes.size() != other.m_nodes.size()) return false;
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            const Node& a = m_nodes[i];
            const Node& b = other.m_nodes[i];
            if (a.x != b.x || a.y != b.y || a.z != b.z) return false;
        }
        return true;
    }

} // namespace Game
