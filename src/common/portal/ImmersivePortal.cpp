// File: src/common/portal/ImmersivePortal.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "ImmersivePortal.hpp"

#include <glm/gtc/matrix_transform.hpp>


#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Game::Immersive {

    namespace {

        constexpr double kEpsilon = 1e-9;

        // Barycentric point-in-triangle, with the triangle grown by
        // `leniency` along its edge normals so a point just outside still
        // counts. Mirrors the mod's Mesh2D.isPointInside leniency.
        bool PointInTriangle(const glm::dvec2& p, const glm::dvec2& a,
                             const glm::dvec2& b, const glm::dvec2& c, double leniency) {
            auto edge = [&](const glm::dvec2& e0, const glm::dvec2& e1) {
                const glm::dvec2 d = e1 - e0;
                const double len = glm::length(d);
                if (len < kEpsilon) return 0.0;
                // Signed distance from p to the edge line, positive on the
                // interior side for a counter-clockwise triangle.
                return ((p.x - e0.x) * d.y - (p.y - e0.y) * d.x) / len;
            };
            const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            const double sign = area >= 0.0 ? -1.0 : 1.0;
            return sign * edge(a, b) <= leniency &&
                   sign * edge(b, c) <= leniency &&
                   sign * edge(c, a) <= leniency;
        }

    } // namespace

    // ── PortalShape ────────────────────────────────────────────────────────

    bool PortalShape::ContainsLocal(double u, double v, double halfWidth, double halfHeight,
                                    double leniency) const {
        if (type == Type::Rectangle) {
            return std::abs(u) <= halfWidth + leniency && std::abs(v) <= halfHeight + leniency;
        }
        // The mesh is clipped to the rectangle by construction; test that
        // first so a point far outside skips the triangle walk.
        if (std::abs(u) > halfWidth + leniency || std::abs(v) > halfHeight + leniency) return false;
        const glm::dvec2 p(u, v);
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
            if (PointInTriangle(p, glm::dvec2(vertices[i0]), glm::dvec2(vertices[i1]),
                                glm::dvec2(vertices[i2]), leniency)) {
                return true;
            }
        }
        return false;
    }

    void PortalShape::LocalBounds(double halfWidth, double halfHeight,
                                  glm::dvec2& outMin, glm::dvec2& outMax) const {
        if (type == Type::Rectangle || vertices.empty()) {
            outMin = glm::dvec2(-halfWidth, -halfHeight);
            outMax = glm::dvec2(halfWidth, halfHeight);
            return;
        }
        outMin = glm::dvec2(vertices[0]);
        outMax = glm::dvec2(vertices[0]);
        for (const auto& v : vertices) {
            outMin = glm::min(outMin, glm::dvec2(v));
            outMax = glm::max(outMax, glm::dvec2(v));
        }
        outMin = glm::max(outMin, glm::dvec2(-halfWidth, -halfHeight));
        outMax = glm::min(outMax, glm::dvec2(halfWidth, halfHeight));
    }

    PortalShape PortalShape::MirroredU(double scale) const {
        PortalShape out;
        out.type = type;
        if (type == Type::Rectangle) return out;
        out.vertices.reserve(vertices.size());
        for (const auto& v : vertices) {
            out.vertices.emplace_back(static_cast<float>(-v.x * scale),
                                      static_cast<float>(v.y * scale));
        }
        out.indices.reserve(indices.size());
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            // Mirroring flips the winding; swap two indices to restore it.
            out.indices.push_back(indices[i]);
            out.indices.push_back(indices[i + 2]);
            out.indices.push_back(indices[i + 1]);
        }
        return out;
    }

    // ── Portal: frame ──────────────────────────────────────────────────────

    glm::dvec3 Portal::Normal() const {
        const glm::dvec3 n = glm::cross(axisW, axisH);
        const double len = glm::length(n);
        return len > kEpsilon ? n / len : glm::dvec3(0.0, 0.0, 1.0);
    }

    Game::Math::ChunkPos Portal::OriginChunk() const {
        return Game::Math::ChunkPos{
            static_cast<int32_t>(std::floor(origin.x)) >> 4,
            static_cast<int32_t>(std::floor(origin.z)) >> 4,
        };
    }

    glm::dvec3 Portal::WorldToLocal(const glm::dvec3& p) const {
        const glm::dvec3 d = p - origin;
        return glm::dvec3(glm::dot(d, axisW), glm::dot(d, axisH), glm::dot(d, Normal()));
    }

    glm::dvec3 Portal::LocalToWorld(double u, double v) const {
        return origin + axisW * u + axisH * v;
    }

    double Portal::SignedDistanceToPlane(const glm::dvec3& p) const {
        return glm::dot(p - origin, Normal());
    }

    bool Portal::IsInProjection(const glm::dvec3& p, double leniency) const {
        const glm::dvec3 local = WorldToLocal(p);
        return shape.ContainsLocal(local.x, local.y, HalfWidth(), HalfHeight(), leniency);
    }

    // ── Portal: transform ──────────────────────────────────────────────────

    bool Portal::IntersectsBox(const glm::dvec3& boxMin, const glm::dvec3& boxMax,
                               double leniency) const {
        const glm::dvec3 n = Normal();
        double dMin = 1e300, dMax = -1e300;
        glm::dvec2 uvMin(1e300), uvMax(-1e300);
        for (int i = 0; i < 8; ++i) {
            const glm::dvec3 c((i & 1) ? boxMax.x : boxMin.x,
                               (i & 2) ? boxMax.y : boxMin.y,
                               (i & 4) ? boxMax.z : boxMin.z);
            const glm::dvec3 d = c - origin;
            const double depth = glm::dot(d, n);
            dMin = std::min(dMin, depth);
            dMax = std::max(dMax, depth);
            const glm::dvec2 uv(glm::dot(d, axisW), glm::dot(d, axisH));
            uvMin = glm::min(uvMin, uv);
            uvMax = glm::max(uvMax, uv);
        }
        if (dMin > 0.0 || dMax < 0.0) return false;   // wholly on one side
        glm::dvec2 outMin, outMax;
        shape.LocalBounds(HalfWidth(), HalfHeight(), outMin, outMax);
        return uvMax.x >= outMin.x - leniency && uvMin.x <= outMax.x + leniency &&
               uvMax.y >= outMin.y - leniency && uvMin.y <= outMax.y + leniency;
    }

    glm::dvec3 Portal::TransformLocalVecNonScale(const glm::dvec3& v) const {
        if (IsMirror()) {
            const glm::dvec3 n = Normal();
            return v - 2.0 * glm::dot(v, n) * n;
        }
        return rotation * v;
    }

    glm::dvec3 Portal::TransformLocalVec(const glm::dvec3& v) const {
        if (IsMirror()) return TransformLocalVecNonScale(v);
        return rotation * (v * scale);
    }

    glm::dvec3 Portal::TransformPoint(const glm::dvec3& p) const {
        // A mirror's destination IS its origin; using `destination` here
        // would let a stale field break the reflection.
        const glm::dvec3 target = IsMirror() ? origin : destination;
        return target + TransformLocalVec(p - origin);
    }

    glm::dvec3 Portal::InverseTransformLocalVec(const glm::dvec3& v) const {
        if (IsMirror()) return TransformLocalVecNonScale(v);   // a reflection is its own inverse
        const double s = scale > kEpsilon ? scale : 1.0;
        return glm::conjugate(rotation) * v / s;
    }

    glm::dvec3 Portal::InverseTransformPoint(const glm::dvec3& p) const {
        const glm::dvec3 target = IsMirror() ? origin : destination;
        return origin + InverseTransformLocalVec(p - target);
    }

    glm::dmat4 Portal::TransformMatrix() const {
        if (IsMirror()) {
            const glm::dvec3 n = Normal();
            const glm::dmat3 reflect = glm::dmat3(1.0) - 2.0 * glm::outerProduct(n, n);
            return glm::translate(glm::dmat4(1.0), origin) * glm::dmat4(reflect) *
                   glm::translate(glm::dmat4(1.0), -origin);
        }
        return glm::translate(glm::dmat4(1.0), destination) *
               glm::mat4_cast(rotation) *
               glm::scale(glm::dmat4(1.0), glm::dvec3(scale)) *
               glm::translate(glm::dmat4(1.0), -origin);
    }

    glm::dvec3 Portal::ContentDirection() const {
        return TransformLocalVecNonScale(-Normal());
    }

    HalfSpace Portal::InnerClipPlane() const {
        return HalfSpace{ IsMirror() ? origin : destination, ContentDirection() };
    }

    HalfSpace Portal::OuterClipPlane() const {
        return HalfSpace{ origin, Normal() };
    }

    std::optional<Portal::SegmentHit> Portal::RaytraceSegment(const glm::dvec3& from,
                                                              const glm::dvec3& to,
                                                              double leniency) const {
        const glm::dvec3 n = Normal();
        const double d0 = glm::dot(from - origin, n);
        const double d1 = glm::dot(to - origin, n);
        // Front to back only. A segment that starts behind the surface, or
        // never reaches it, is not a crossing — the mod tests exactly this
        // (`localFrom.z > 0 && localTo.z <= 0`).
        if (!(d0 > 0.0 && d1 <= 0.0)) return std::nullopt;
        const double denom = d0 - d1;
        if (denom < kEpsilon) return std::nullopt;
        const double t = d0 / denom;
        const glm::dvec3 point = from + (to - from) * t;
        const glm::dvec3 local = WorldToLocal(point);
        if (!shape.ContainsLocal(local.x, local.y, HalfWidth(), HalfHeight(), leniency)) {
            return std::nullopt;
        }
        return SegmentHit{ t, point };
    }

    void Portal::Corners(glm::dvec3 out[4]) const {
        const glm::dvec3 w = axisW * HalfWidth();
        const glm::dvec3 h = axisH * HalfHeight();
        out[0] = origin - w - h;
        out[1] = origin + w - h;
        out[2] = origin + w + h;
        out[3] = origin - w + h;
    }

    void Portal::BoundingBox(glm::dvec3& outMin, glm::dvec3& outMax, double thickness) const {
        glm::dvec3 c[4];
        Corners(c);
        outMin = c[0];
        outMax = c[0];
        for (int i = 1; i < 4; ++i) {
            outMin = glm::min(outMin, c[i]);
            outMax = glm::max(outMax, c[i]);
        }
        const glm::dvec3 pad = glm::abs(Normal()) * thickness;
        outMin -= pad;
        outMax += pad;
    }

    // ── Portal: cluster construction ───────────────────────────────────────

    Portal Portal::MakeReverse() const {
        Portal r = *this;
        r.id = kInvalidPortalId;
        r.reversePortalId = r.flippedPortalId = r.parallelPortalId = kInvalidPortalId;
        if (IsMirror()) return r;   // a mirror is its own reverse

        r.dimension     = destDimension;
        r.destDimension = dimension;
        r.origin        = destination;
        r.destination   = origin;
        // The reverse faces back the way this one leads: its normal is this
        // one's content direction, which negating axisW achieves.
        r.axisW    = TransformLocalVecNonScale(-axisW);
        r.axisH    = TransformLocalVecNonScale(axisH);
        r.width    = width  * scale;
        r.height   = height * scale;
        r.rotation = glm::conjugate(rotation);
        r.scale    = scale > kEpsilon ? 1.0 / scale : 1.0;
        r.shape    = shape.MirroredU(scale);
        return r;
    }

    Portal Portal::MakeFlipped() const {
        Portal f = *this;
        f.id = kInvalidPortalId;
        f.reversePortalId = f.flippedPortalId = f.parallelPortalId = kInvalidPortalId;
        f.axisW = -axisW;
        f.shape = shape.MirroredU(1.0);
        return f;
    }

    // ── Portal: validation ─────────────────────────────────────────────────

    bool Portal::IsValidGeometry() const {
        constexpr double kUnitTolerance = 1e-4;
        if (!(width > 0.0) || !(height > 0.0)) return false;
        if (!(scale > 0.0) || !std::isfinite(scale)) return false;
        if (std::abs(glm::length(axisW) - 1.0) > kUnitTolerance) return false;
        if (std::abs(glm::length(axisH) - 1.0) > kUnitTolerance) return false;
        if (std::abs(glm::dot(axisW, axisH)) > kUnitTolerance) return false;
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) return false;
        if (!std::isfinite(destination.x) || !std::isfinite(destination.y) ||
            !std::isfinite(destination.z)) return false;
        if (shape.type == PortalShape::Type::Mesh) {
            if (shape.indices.size() % 3 != 0 || shape.vertices.empty()) return false;
            for (uint32_t i : shape.indices) if (i >= shape.vertices.size()) return false;
        }
        return true;
    }

    bool Portal::Orthonormalize() {
        const double lw = glm::length(axisW);
        if (lw < kEpsilon) return false;
        axisW /= lw;
        // Gram–Schmidt H against W.
        axisH -= axisW * glm::dot(axisH, axisW);
        const double lh = glm::length(axisH);
        if (lh < kEpsilon) return false;
        axisH /= lh;
        rotation = glm::normalize(rotation);
        return true;
    }

    std::string Portal::Describe() const {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "#%u %s %.1fx%.1f %s (%.1f, %.1f, %.1f) -> %s (%.1f, %.1f, %.1f)%s%s",
                      id, PortalKindName(kind), width, height,
                      std::string(Game::DimensionName(dimension)).c_str(),
                      origin.x, origin.y, origin.z,
                      std::string(Game::DimensionName(destDimension)).c_str(),
                      destination.x, destination.y, destination.z,
                      scale != 1.0 ? " scaled" : "",
                      reversePortalId != kInvalidPortalId ? " bi-way" : "");
        return buf;
    }

    bool Portal::operator==(const Portal& o) const {
        return id == o.id && kind == o.kind && flags == o.flags &&
               dimension == o.dimension && origin == o.origin &&
               axisW == o.axisW && axisH == o.axisH &&
               width == o.width && height == o.height &&
               destDimension == o.destDimension && destination == o.destination &&
               rotation == o.rotation && scale == o.scale &&
               specificPlayerId == o.specificPlayerId &&
               reversePortalId == o.reversePortalId &&
               flippedPortalId == o.flippedPortalId &&
               parallelPortalId == o.parallelPortalId &&
               shape == o.shape && tag == o.tag;
    }

    const char* PortalKindName(PortalKind kind) {
        switch (kind) {
            case PortalKind::Generic:      return "portal";
            case PortalKind::NetherPortal: return "nether_portal";
            case PortalKind::EndPortal:    return "end_portal";
            case PortalKind::PortalGun:    return "gun_portal";
            case PortalKind::Mirror:       return "mirror";
        }
        return "portal";
    }

} // namespace Game::Immersive

#endif // ENABLE_IMMERSIVE_PORTALS
