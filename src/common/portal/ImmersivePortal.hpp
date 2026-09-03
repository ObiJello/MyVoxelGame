// File: src/common/portal/ImmersivePortal.hpp
//
// The immersive portal: a planar surface in one dimension that shows, and
// leads to, another place in the same or another dimension.
//
// This is the data model the Immersive Portals mod calls `Portal` (an entity
// there; a plain record here, because nothing about it ticks or has physics
// of its own). Everything the rest of the system needs is derived from these
// fields, and in exactly the mod's terms so its math carries over verbatim:
//
//   origin            centre of the surface, world space
//   axisW, axisH      unit vectors spanning the surface; the NORMAL is
//                     cross(axisW, axisH), and a viewer standing on the
//                     positive side of it sees through
//   width, height     extents along axisW / axisH (the rectangle; a Mesh
//                     shape may cut any flat outline out of it)
//   destination       where `origin` maps to, in destDimension
//   rotation, scale   how directions transform on the way through; a plain
//                     nether portal has identity and 1.0
//
// The full transform is   T(p) = destination + R · (s · (p − origin))
// and the camera seen from the other side is the viewer's camera pushed
// through T. A mirror is the same record with `destination == origin` and the
// reflection across its own plane in place of R·s.
//
// Cluster links: the mod represents a two-way portal as FOUR records (each
// side has a front and a back face) and keeps them in step through
// reverse/flipped/parallel ids. Same here — MakeReverse and MakeFlipped build
// the partners; the registry assigns and stores the links.
//
// Wire and save formats live next to the packets (ImmersivePortalPackets.hpp)
// and the server registry respectively; this header is pure geometry.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game::Immersive {

    using PortalId = uint32_t;
    inline constexpr PortalId kInvalidPortalId = 0;

    // What created the portal and what rules it plays by. Wire-stable — the
    // client reads it to pick a look (nether portals get purple particles,
    // gun portals get their rim) and later phases key behaviour on it.
    enum class PortalKind : uint8_t {
        Generic      = 0,   // commands, API
        NetherPortal = 1,   // an obsidian frame (phase 6)
        EndPortal    = 2,
        PortalGun    = 3,   // the Valve-style pair (phase 8)
        Mirror       = 4,
    };

    // Behaviour bits. Named after the mod's fields so its docs apply.
    namespace PortalFlag {
        inline constexpr uint32_t Teleportable         = 1u << 0;  // entities may cross
        inline constexpr uint32_t Visible              = 1u << 1;  // rendered at all
        inline constexpr uint32_t Interactable         = 1u << 2;  // blocks may be targeted through it
        inline constexpr uint32_t FuseView             = 1u << 3;  // no depth/sky reset: far side reads as continuous
        inline constexpr uint32_t RenderPlayer         = 1u << 4;  // the viewer's own body shows through it
        inline constexpr uint32_t CrossPortalCollision = 1u << 5;  // half-through entities collide on both sides
        inline constexpr uint32_t Mirror               = 1u << 6;  // reflection instead of a transform
        // A world-sized surface (a wrap border, a dimension-stack seam):
        // not tied to a chunk, sent to every client in its dimension, never
        // saved (the world options rebuild it), loaders follow the player's
        // image through it rather than its centre.
        inline constexpr uint32_t Global               = 1u << 7;

        inline constexpr uint32_t Default =
            Teleportable | Visible | Interactable | RenderPlayer | CrossPortalCollision;
    }

    // The outline of the surface in its own (u, v) plane — u along axisW, v
    // along axisH, both centred on the origin and measured in blocks.
    //
    // Rectangle covers the whole width×height. Mesh is a triangle list inside
    // that rectangle: the shape an arbitrary obsidian loop cuts out, or a
    // round portal. Rendering, raytracing and the collision footprint all go
    // through ContainsLocal so the two agree.
    struct PortalShape {
        enum class Type : uint8_t { Rectangle = 0, Mesh = 1 };

        Type type = Type::Rectangle;

        // Mesh only. Vertices in local (u, v); indices form triangles.
        std::vector<glm::vec2> vertices;
        std::vector<uint32_t>  indices;

        bool IsRectangle() const { return type == Type::Rectangle; }

        // `leniency` grows the shape outward by that many blocks — the
        // teleport test uses a small one so an entity skimming the edge of
        // the frame is still counted as inside, as the mod does.
        bool ContainsLocal(double u, double v, double halfWidth, double halfHeight,
                           double leniency = 0.0) const;

        // Bounds of the shape in local units (a Mesh may be smaller than the
        // rectangle it sits in; never larger).
        void LocalBounds(double halfWidth, double halfHeight,
                         glm::dvec2& outMin, glm::dvec2& outMax) const;

        // The shape as seen from the other side of a portal whose axisW was
        // negated: u mirrored, and triangles re-wound so the front face stays
        // the front face. `scale` multiplies both axes (a reverse portal of a
        // scaling one is that much larger).
        PortalShape MirroredU(double scale) const;

        bool operator==(const PortalShape& o) const {
            return type == o.type && vertices == o.vertices && indices == o.indices;
        }
    };

    // A plane with a kept side: points p with dot(p − point, normal) > 0.
    struct HalfSpace {
        glm::dvec3 point{0.0};
        glm::dvec3 normal{0.0, 0.0, 1.0};

        double SignedDistance(const glm::dvec3& p) const { return glm::dot(p - point, normal); }
        bool   Contains(const glm::dvec3& p) const { return SignedDistance(p) > 0.0; }
        // As the vec4 the shaders take: (n.xyz, −n·point), positive = kept.
        glm::vec4 AsClipPlane() const {
            return glm::vec4(glm::vec3(normal), static_cast<float>(-glm::dot(normal, point)));
        }
    };

    struct Portal {
        PortalId   id    = kInvalidPortalId;
        PortalKind kind  = PortalKind::Generic;
        uint32_t   flags = PortalFlag::Default;

        // This side.
        Game::DimensionId dimension = Game::DimensionId::Overworld;
        glm::dvec3 origin{0.0};
        glm::dvec3 axisW{1.0, 0.0, 0.0};
        glm::dvec3 axisH{0.0, 1.0, 0.0};
        double     width  = 1.0;
        double     height = 2.0;

        // The other side.
        Game::DimensionId destDimension = Game::DimensionId::Overworld;
        glm::dvec3 destination{0.0};
        glm::dquat rotation{1.0, 0.0, 0.0, 0.0};   // identity; applied to directions on the way through
        double     scale = 1.0;                     // > 1 makes the far side larger

        // 0 = every player. Otherwise only this player id sees and uses it
        // (the mod's specificPlayerId; the gun's per-owner portals want it).
        uint32_t specificPlayerId = 0;

        // Cluster partners, kInvalidPortalId when absent. See the file
        // comment; the registry keeps them consistent, this record just
        // carries them.
        PortalId reversePortalId  = kInvalidPortalId;   // at the destination, leads back
        PortalId flippedPortalId  = kInvalidPortalId;   // same place, faces the other way
        PortalId parallelPortalId = kInvalidPortalId;   // reverse of the flipped one

        PortalShape shape;

        // Free-form label: "nether", "gun:blue", a command's name — anything
        // a later system wants to find its own portals by.
        std::string tag;

        // ── Derived geometry ────────────────────────────────────────────
        bool Has(uint32_t flag) const { return (flags & flag) != 0; }
        // How far outside the outline a crossing segment may pierce and
        // still count. A gun portal's oval narrows toward the top, where a
        // standing player's eye passes; the walkable opening in the wall is
        // the whole 1×2, so the crossing must accept the whole 1×2 too.
        double CrossingLeniency() const { return kind == PortalKind::PortalGun ? 0.5 : 0.05; }
        bool IsMirror() const { return Has(PortalFlag::Mirror); }

        double HalfWidth()  const { return width  * 0.5; }
        double HalfHeight() const { return height * 0.5; }

        // Unit normal of the surface. The viewer on its positive side sees
        // through; an entity crosses from positive to negative.
        glm::dvec3 Normal() const;

        // The chunk the surface is anchored in — the one whose watchers are
        // told about it, and whose unload on the client drops it.
        Game::Math::ChunkPos OriginChunk() const;

        // Local frame: (u along axisW, v along axisH, n along the normal).
        glm::dvec3 WorldToLocal(const glm::dvec3& p) const;
        glm::dvec3 LocalToWorld(double u, double v) const;   // on the plane

        double SignedDistanceToPlane(const glm::dvec3& p) const;
        bool   IsInFront(const glm::dvec3& p) const { return SignedDistanceToPlane(p) > 0.0; }

        // Is the point, projected onto the plane, inside the outline?
        bool IsInProjection(const glm::dvec3& p, double leniency = 0.0) const;
        // Does the box straddle the surface: corners on both sides of the
        // plane, and its footprint on the plane overlapping the outline's
        // bounds (grown by `leniency`)? What "an entity in the portal" means.
        bool IntersectsBox(const glm::dvec3& boxMin, const glm::dvec3& boxMax,
                           double leniency = 0.0) const;

        // ── The transform through the portal ─────────────────────────────
        // Directions: rotation and scale (a mirror reflects instead).
        glm::dvec3 TransformLocalVec(const glm::dvec3& v) const;
        // Directions without the scale — for normals, axes, view vectors.
        glm::dvec3 TransformLocalVecNonScale(const glm::dvec3& v) const;
        // Points: origin ↦ destination.
        glm::dvec3 TransformPoint(const glm::dvec3& p) const;
        // The inverse map, destination ↦ origin. What the reverse portal does.
        glm::dvec3 InverseTransformPoint(const glm::dvec3& p) const;
        // TransformPoint as a 4×4 affine matrix (this side ↦ far side), for
        // pushing a camera's view matrix through: view' = view · M⁻¹.
        glm::dmat4 TransformMatrix() const;
        glm::dvec3 InverseTransformLocalVec(const glm::dvec3& v) const;

        // The direction, on the far side, that the visible world lies in:
        // the transformed −normal. Renderers keep everything on that side of
        // the destination plane and clip the rest.
        glm::dvec3 ContentDirection() const;

        // Clip planes in the mod's terms. Inner = what is drawn of the far
        // world (the destination plane, kept side = content). Outer = what
        // is drawn of a body standing in the surface on THIS side.
        HalfSpace InnerClipPlane() const;
        HalfSpace OuterClipPlane() const;

        // Did a point moving from `from` to `to` pass through the surface
        // front-to-back? The one test teleporting, both client and server,
        // is built on.
        struct SegmentHit {
            double     t = 0.0;         // fraction along the segment
            glm::dvec3 point{0.0};      // world position of the crossing
        };
        std::optional<SegmentHit> RaytraceSegment(const glm::dvec3& from, const glm::dvec3& to,
                                                  double leniency = 0.0) const;

        // World-space box around the surface, ±thickness along the normal.
        // The broad-phase test for "might this entity touch the portal".
        void BoundingBox(glm::dvec3& outMin, glm::dvec3& outMax, double thickness = 0.2) const;

        // The four rectangle corners in world space, in axisW/axisH order:
        // (−w,−h), (+w,−h), (+w,+h), (−w,+h).
        void Corners(glm::dvec3 out[4]) const;

        // ── Cluster construction ─────────────────────────────────────────
        // The portal sitting at the destination that leads back here. Its id
        // and links are left at kInvalidPortalId for the registry to fill in.
        Portal MakeReverse() const;
        // The same surface facing the other way (so the far side of a wall
        // sees through it too). Same transform, negated axisW.
        Portal MakeFlipped() const;

        // Both axes unit and orthogonal, positive extents, sane scale.
        bool IsValidGeometry() const;
        // Re-normalises the frame after floating-point drift; false if the
        // axes were degenerate and could not be recovered.
        bool Orthonormalize();

        // One line for logs and chat: id, kind, where, to where.
        std::string Describe() const;

        bool operator==(const Portal& o) const;
        bool operator!=(const Portal& o) const { return !(*this == o); }
    };

    const char* PortalKindName(PortalKind kind);

} // namespace Game::Immersive

#endif // ENABLE_IMMERSIVE_PORTALS
