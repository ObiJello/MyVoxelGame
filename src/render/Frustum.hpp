#pragma once

#include <glm/glm.hpp>
#include <array>

// A simple AABB
struct AABB {
    glm::vec3 min; // world‐space minimum corner
    glm::vec3 max; // world‐space maximum corner
};

// A view frustum represented by 6 planes in world space:
// Each plane is (a, b, c, d) such that ax + by + cz + d >= 0 is "inside."
struct Frustum {
    // Indexing convention: 0=Left, 1=Right, 2=Bottom, 3=Top, 4=Near, 5=Far
    std::array<glm::vec4, 6> planes;

    // Extract planes from a combined projection * view matrix (clip space).
    // After extraction, each plane's (xyz) is normalized and d is scaled accordingly.
    static Frustum FromMatrix(const glm::mat4& m) {
        Frustum f;

        // For OpenGL clip space, we extract planes from the MVP matrix
        // The matrix is in column-major order: m[col][row]
        // We need to access it as m[row][col] for plane extraction

        // Extract the 4 rows of the matrix
        glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
        glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
        glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
        glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);

        // Extract frustum planes:
        // Left   plane: row3 + row0
        // Right  plane: row3 - row0
        // Bottom plane: row3 + row1
        // Top    plane: row3 - row1
        // Near   plane: row3 + row2
        // Far    plane: row3 - row2

        glm::vec4 tempPlanes[6] = {
            row3 + row0,  // Left
            row3 - row0,  // Right
            row3 + row1,  // Bottom
            row3 - row1,  // Top
            row3 + row2,  // Near
            row3 - row2   // Far
        };

        // Normalize each plane
        for (int i = 0; i < 6; ++i) {
            glm::vec3 normal(tempPlanes[i].x, tempPlanes[i].y, tempPlanes[i].z);
            float length = glm::length(normal);
            if (length > 0.0f) {
                f.planes[i] = tempPlanes[i] / length;
            } else {
                // Degenerate plane, shouldn't happen with valid matrices
                f.planes[i] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
            }
        }

        return f;
    }

    // Test if an AABB is at least partially inside (or intersects) the frustum.
    bool IsBoxVisible(const AABB& box) const {
        // For each plane, find the "positive vertex" (furthest point in direction of plane normal)
        // If that vertex is behind the plane, the entire box is outside
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& plane = planes[i];
            glm::vec3 normal(plane.x, plane.y, plane.z);

            // Find the positive vertex (furthest point along plane normal)
            glm::vec3 positiveVertex;
            positiveVertex.x = (normal.x >= 0.0f) ? box.max.x : box.min.x;
            positiveVertex.y = (normal.y >= 0.0f) ? box.max.y : box.min.y;
            positiveVertex.z = (normal.z >= 0.0f) ? box.max.z : box.min.z;

            // Test if positive vertex is behind this plane
            float distance = glm::dot(normal, positiveVertex) + plane.w;
            if (distance < 0.0f) {
                return false; // Box is completely outside this plane
            }
        }
        return true; // Box intersects or is inside frustum
    }
};