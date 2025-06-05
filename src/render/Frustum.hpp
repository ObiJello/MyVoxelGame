#pragma once

#include <glm/glm.hpp>
#include <array>

// A simple AABB
struct AABB {
    glm::vec3 min; // world‐space minimum corner
    glm::vec3 max; // world‐space maximum corner
};

// A view frustum represented by 6 planes in world space:
// Each plane is (a, b, c, d) such that ax + by + cz + d >= 0 is “inside.”
struct Frustum {
    // Indexing convention: 0=Left, 1=Right, 2=Bottom, 3=Top, 4=Near, 5=Far
    std::array<glm::vec4, 6> planes;

    // Extract planes from a combined projection * view matrix (clip space).
    // After extraction, each plane’s (xyz) is normalized and d is scaled accordingly.
    static Frustum FromMatrix(const glm::mat4& m) {
        Frustum f;
        // m is column‐major, so m[c][r] is element at row r, col c.
        // Left   plane:  m[3] + m[0]
        // Right  plane:  m[3] − m[0]
        // Bottom plane:  m[3] + m[1]
        // Top    plane:  m[3] − m[1]
        // Near   plane:  m[3] + m[2]
        // Far    plane:  m[3] − m[2]
        for (int i = 0; i < 6; ++i) {
            glm::vec4 p;
            switch (i) {
            case 0: p = m[3] + m[0]; break; // Left
            case 1: p = m[3] - m[0]; break; // Right
            case 2: p = m[3] + m[1]; break; // Bottom
            case 3: p = m[3] - m[1]; break; // Top
            case 4: p = m[3] + m[2]; break; // Near
            default: p = m[3] - m[2]; break; // Far
            }
            // Normalize the plane: divide (a,b,c,d) by length of (a,b,c)
            float length = glm::length(glm::vec3(p));
            f.planes[i] = p / length;
        }
        return f;
    }

    // Test if an AABB is at least partially inside (or intersects) the frustum.
    bool IsBoxVisible(const AABB& box) const {
        // For each plane, find the “most negative” corner of the box.
        // If that corner is outside (plane dot corner + d < 0), the box is completely outside.
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& P = planes[i];
            glm::vec3 normal(P.x, P.y, P.z);

            glm::vec3 negativeCorner = box.min;
            if (normal.x >= 0.0f) negativeCorner.x = box.max.x;
            if (normal.y >= 0.0f) negativeCorner.y = box.max.y;
            if (normal.z >= 0.0f) negativeCorner.z = box.max.z;

            float dist = glm::dot(normal, negativeCorner) + P.w;
            if (dist < 0.0f) {
                return false; // entirely outside this plane
            }
        }
        return true;
    }
};