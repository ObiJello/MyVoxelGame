// File: src/client/renderer/core/Frustum.hpp
#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>

// A simple AABB
struct AABB {
    glm::vec3 min; // world‐space minimum corner
    glm::vec3 max; // world‐space maximum corner
};

// Tri-state frustum test result for hierarchical culling
enum class FrustumResult { Outside, Intersect, Inside };

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

    // A "portal frustum": the four side planes pass through `eye` and the
    // edges of a quad (the far side of a portal surface, in the order the
    // portal's corners come in), so only what is visible THROUGH the quad
    // passes. Near and far planes are taken from `base`. This is the
    // Immersive Portals mod's FrustumCuller inner culling.
    static Frustum ThroughQuad(const glm::vec3& eye, const glm::vec3 corners[4], const Frustum& base) {
        Frustum f = base;
        const glm::vec3 center = (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25f;
        // A quad seen nearly edge-on is a sliver on screen: its centre lies
        // within rounding of the very planes being built, so the inward
        // test below picks a side at random, a flipped plane culls the
        // whole far view, and the portal shows its wall instead of the
        // far world (seen from the side, at one exact angle). The base
        // frustum is the safe answer there — a sliver costs nothing extra.
        {
            const glm::vec3 toCenter = center - eye;
            const float dist = glm::length(toCenter);
            const glm::vec3 quadNormal = glm::cross(corners[1] - corners[0], corners[3] - corners[0]);
            const float quadLen = glm::length(quadNormal);
            if (dist < 1e-6f || quadLen < 1e-9f) return base;
            constexpr float kMinCosine = 0.02f;   // about one degree off edge-on
            if (std::abs(glm::dot(quadNormal / quadLen, toCenter / dist)) < kMinCosine) return base;
        }
        for (int i = 0; i < 4; ++i) {
            const glm::vec3 a = corners[i] - eye;
            const glm::vec3 b = corners[(i + 1) % 4] - eye;
            glm::vec3 n = glm::cross(a, b);
            const float len = glm::length(n);
            if (len < 1e-9f) continue;          // degenerate edge: keep the base plane
            n /= len;
            // Inward: the two corners NOT on this edge must be on the
            // positive side. Judged by the one farther from the plane —
            // the centre (a quarter of the way to the plane) was too close
            // a call at grazing angles.
            const float d2 = glm::dot(n, corners[(i + 2) % 4] - eye);
            const float d3 = glm::dot(n, corners[(i + 3) % 4] - eye);
            const float sideSign = std::abs(d2) >= std::abs(d3) ? d2 : d3;
            if (sideSign < 0.0f) n = -n;
            f.planes[i] = glm::vec4(n, -glm::dot(n, eye));
        }
        return f;
    }

    // The part of a portal rectangle this frustum can see: `corners` in
    // cyclic order with corners[1]-corners[0] ⟂ corners[3]-corners[0] (a
    // portal's Corners()). The rectangle is clipped (Sutherland–Hodgman, in
    // double) against the four side planes and the near plane, and the
    // clipped polygon is bounded by a rectangle in the quad's OWN axes,
    // grown by `margin` blocks and clamped to the quad — a conservative
    // superset of the visible part, still a rectangle, so ThroughQuad can
    // build a portal frustum from it. A portal partly off-screen then
    // culls its far view to the on-screen part (Source's portal frustum
    // from the visible portal rectangle). False — `out` untouched — when
    // nothing of it is inside or the quad is degenerate; callers keep the
    // whole quad then.
    bool VisibleSubRect(const glm::dvec3 corners[4], double margin, glm::dvec3 out[4]) const {
        const glm::dvec3 e1 = corners[1] - corners[0];
        const glm::dvec3 e2 = corners[3] - corners[0];
        const double l1 = glm::dot(e1, e1);
        const double l2 = glm::dot(e2, e2);
        if (l1 < 1e-12 || l2 < 1e-12) return false;
        constexpr int kMax = 16;
        std::array<glm::dvec3, kMax> poly{}, next{};
        int count = 4;
        for (int i = 0; i < 4; ++i) poly[static_cast<size_t>(i)] = corners[i];
        const int clipPlanes[5] = { 0, 1, 2, 3, 4 };   // left, right, bottom, top, near
        for (int pi : clipPlanes) {
            const glm::dvec4 pl(planes[static_cast<size_t>(pi)]);
            auto dist = [&](const glm::dvec3& p) { return pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w; };
            int n = 0;
            for (int i = 0; i < count && n < kMax - 1; ++i) {
                const glm::dvec3& a = poly[static_cast<size_t>(i)];
                const glm::dvec3& b = poly[static_cast<size_t>((i + 1) % count)];
                const double da = dist(a), db = dist(b);
                if (da >= 0.0) next[static_cast<size_t>(n++)] = a;
                if ((da >= 0.0) != (db >= 0.0) && n < kMax) {
                    const double t = da / (da - db);
                    next[static_cast<size_t>(n++)] = a + (b - a) * t;
                }
            }
            count = n;
            if (count == 0) return false;
            poly = next;
        }
        double s0 = 1.0, s1 = 0.0, t0 = 1.0, t1 = 0.0;
        for (int i = 0; i < count; ++i) {
            const glm::dvec3 d = poly[static_cast<size_t>(i)] - corners[0];
            const double s = glm::dot(d, e1) / l1;
            const double t = glm::dot(d, e2) / l2;
            s0 = std::min(s0, s); s1 = std::max(s1, s);
            t0 = std::min(t0, t); t1 = std::max(t1, t);
        }
        const double ms = margin / std::sqrt(l1);
        const double mt = margin / std::sqrt(l2);
        s0 = std::clamp(s0 - ms, 0.0, 1.0); s1 = std::clamp(s1 + ms, 0.0, 1.0);
        t0 = std::clamp(t0 - mt, 0.0, 1.0); t1 = std::clamp(t1 + mt, 0.0, 1.0);
        if (s1 - s0 < 1e-6 || t1 - t0 < 1e-6) return false;
        out[0] = corners[0] + e1 * s0 + e2 * t0;
        out[1] = corners[0] + e1 * s1 + e2 * t0;
        out[2] = corners[0] + e1 * s1 + e2 * t1;
        out[3] = corners[0] + e1 * s0 + e2 * t1;
        return true;
    }

    // Test if an AABB is at least partially inside (or intersects) the frustum.
    bool IsBoxVisible(const AABB& box) const {
        return IsBoxVisible(box.min, box.max);
    }
    
    // Optimized overload that takes min/max directly to avoid AABB object creation
    bool IsBoxVisible(const glm::vec3& boxMin, const glm::vec3& boxMax) const {
        // For each plane, find the "positive vertex" (furthest point in direction of plane normal)
        // If that vertex is behind the plane, the entire box is outside
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& plane = planes[i];
            glm::vec3 normal(plane.x, plane.y, plane.z);

            // Find the positive vertex (furthest point along plane normal)
            glm::vec3 positiveVertex;
            positiveVertex.x = (normal.x >= 0.0f) ? boxMax.x : boxMin.x;
            positiveVertex.y = (normal.y >= 0.0f) ? boxMax.y : boxMin.y;
            positiveVertex.z = (normal.z >= 0.0f) ? boxMax.z : boxMin.z;

            // Test if positive vertex is behind this plane
            // Use small negative epsilon (half a block) to prevent boundary flickering
            float distance = glm::dot(normal, positiveVertex) + plane.w;
            if (distance < -0.5f) {
                return false; // Box is completely outside this plane
            }
        }
        return true; // Box intersects or is inside frustum
    }

    // The rows of a chunk column that pass IsBoxVisible, as a closed
    // interval [lo, hi] of section indices (empty when hi < lo). The column
    // is [minX, minX+16) x [minZ, minZ+16), row s is [minY0 + 16 s,
    // minY0 + 16 s + 16), rows 0..rowCount-1.
    //
    // Same predicate as IsBoxVisible on every row's box: for each plane the
    // positive vertex's x and z do not depend on the row, so the plane's
    // x/z/w contribution is one number per column and the row enters only
    // through ny * (top or bottom of the row). A plane with ny > 0 therefore
    // bounds the rows from below, ny < 0 from above, ny == 0 admits all rows
    // or none. Six planes, six bounds, one interval — where the per-row test
    // did six plane tests for each of the 24 rows of every column on the
    // frustum's edge, which was the main cost of the frustum filter
    // (Instruments, 2026-09-04: TestAABB + IsBoxVisible + the vec3
    // constructor = 58% of PrepareVisibleSections).
    void SectionRowRange(float minX, float minZ, float minY0, int rowCount,
                         int& lo, int& hi) const {
        lo = 0;
        hi = rowCount - 1;
        for (int i = 0; i < 6 && lo <= hi; ++i) {
            const glm::vec4& plane = planes[i];
            const float px = (plane.x >= 0.0f) ? minX + 16.0f : minX;
            const float pz = (plane.z >= 0.0f) ? minZ + 16.0f : minZ;
            const float cxz = plane.x * px + plane.z * pz + plane.w;
            if (plane.y > 0.0f) {
                // Passes iff cxz + ny * (minY0 + 16 (s + 1)) >= -0.5
                // Clamped before the cast: a near-horizontal plane (ny of
                // 1e-7) divides to a value no int can hold.
                const float sMin = std::clamp(((-0.5f - cxz) / plane.y - minY0) / 16.0f - 1.0f,
                                              -1.0f, 4096.0f);
                lo = std::max(lo, static_cast<int>(std::ceil(sMin)));
            } else if (plane.y < 0.0f) {
                // Passes iff cxz + ny * (minY0 + 16 s) >= -0.5; ny < 0 flips it
                const float sMax = std::clamp(((-0.5f - cxz) / plane.y - minY0) / 16.0f,
                                              -2.0f, 4096.0f);
                hi = std::min(hi, static_cast<int>(std::floor(sMax)));
            } else if (cxz < -0.5f) {
                hi = lo - 1;   // the whole column is behind this plane
            }
        }
    }

    // Tri-state AABB test: Outside / Intersect / Inside.
    // Used for hierarchical culling — if a parent AABB is fully Inside,
    // children can skip per-element frustum tests entirely.
    FrustumResult TestAABB(const glm::vec3& boxMin, const glm::vec3& boxMax) const {
        bool allInside = true;

        for (int i = 0; i < 6; ++i) {
            const glm::vec4& plane = planes[i];
            glm::vec3 normal(plane.x, plane.y, plane.z);

            // Positive vertex (furthest along plane normal)
            glm::vec3 pVertex;
            pVertex.x = (normal.x >= 0.0f) ? boxMax.x : boxMin.x;
            pVertex.y = (normal.y >= 0.0f) ? boxMax.y : boxMin.y;
            pVertex.z = (normal.z >= 0.0f) ? boxMax.z : boxMin.z;

            float pDist = glm::dot(normal, pVertex) + plane.w;
            if (pDist < -0.5f) {
                return FrustumResult::Outside;
            }

            // Negative vertex (closest along plane normal)
            glm::vec3 nVertex;
            nVertex.x = (normal.x >= 0.0f) ? boxMin.x : boxMax.x;
            nVertex.y = (normal.y >= 0.0f) ? boxMin.y : boxMax.y;
            nVertex.z = (normal.z >= 0.0f) ? boxMin.z : boxMax.z;

            float nDist = glm::dot(normal, nVertex) + plane.w;
            if (nDist < -0.5f) {
                // Negative vertex is outside this plane, so the box
                // straddles the plane — it's not fully inside.
                allInside = false;
            }
        }

        return allInside ? FrustumResult::Inside : FrustumResult::Intersect;
    }
};