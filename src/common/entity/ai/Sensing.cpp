// File: src/common/entity/ai/Sensing.cpp
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <cmath>

namespace Game {

    void Sensing::Tick() {
        m_seen.clear();
        m_unseen.clear();
    }

    bool Sensing::HasLineOfSight(const Entity& target) {
        const int32_t id = target.GetId();
        if (m_seen.count(id))   return true;
        if (m_unseen.count(id)) return false;

        const bool visible = ComputeLineOfSight(target);
        if (visible) m_seen.insert(id); else m_unseen.insert(id);
        return visible;
    }

    bool Sensing::ComputeLineOfSight(const Entity& target) const {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        const IBlockAccess* blocks = level->Blocks();
        if (!blocks) return false;

        // MC clips eye-to-eye with ClipContext.Block/COLLIDER. This is a DDA
        // over the same segment: step in fixed increments and stop at the first
        // block with a collision shape. It is deliberately a block test and not
        // a shape test — MC uses COLLIDER here too, so a mob can see over a
        // fence but not through a wall.
        const glm::dvec3 from = m_mob->GetEyePosition();
        const glm::dvec3 to   = target.GetEyePosition();
        const glm::dvec3 delta = to - from;

        const double distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (distance < 1.0e-4) return true;

        // Quarter-block steps: fine enough that a 1-block wall is never missed,
        // coarse enough that a 32-block sight line is ~128 samples.
        const int steps = static_cast<int>(std::ceil(distance * 4.0));
        const glm::dvec3 step = delta / static_cast<double>(steps);

        glm::dvec3 p = from;
        for (int i = 1; i < steps; ++i) {
            p += step;
            const int bx = static_cast<int>(std::floor(p.x));
            const int by = static_cast<int>(std::floor(p.y));
            const int bz = static_cast<int>(std::floor(p.z));
            if (BlockRegistry::HasCollision(blocks->GetBlock(bx, by, bz))) return false;
        }
        return true;
    }

} // namespace Game
