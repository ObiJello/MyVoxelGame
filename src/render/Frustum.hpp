// File: src/render/Frustum.hpp
#pragma once

#include <glm/glm.hpp>

namespace Render {

    // Axis-Aligned Bounding Box: defined by min/max corners in world space
    struct AABB {
        glm::vec3 min; // minimum (x,y,z)
        glm::vec3 max; // maximum (x,y,z)

        // Test if this AABB intersects another AABB
        bool Intersects(const AABB& other) const {
            return (min.x <= other.max.x && max.x >= other.min.x) &&
                   (min.y <= other.max.y && max.y >= other.min.y) &&
                   (min.z <= other.max.z && max.z >= other.min.z);
        }
    };

    // A simple Ray: origin + direction, both in world space
    struct Ray {
        glm::vec3 origin;
        glm::vec3 direction; // should be normalized

        // Compute point along the ray at parameter t
        inline glm::vec3 At(float t) const {
            return origin + t * direction;
        }
    };

    // (Later, you’ll add frustum‐plane tests, but these basics let you define
    // chunk AABBs and test them against the camera frustum.)
}
