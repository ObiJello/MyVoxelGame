// File: src/client/renderer/portal/PortalCameraTransform.cpp
// See PortalCameraTransform.hpp for scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalCameraTransform.hpp"

#include <algorithm>
#include <cmath>

namespace Render::PortalTransform {

    glm::dmat4 PortalToWorld(const Client::ClientPortal& p) {
        glm::dmat4 m(1.0);
        m[0] = glm::dvec4(p.right,  0.0);
        m[1] = glm::dvec4(p.upDir,  0.0);
        m[2] = glm::dvec4(p.normal, 0.0);
        m[3] = glm::dvec4(p.origin, 1.0);
        return m;
    }

    glm::dmat4 WorldToPortal(const Client::ClientPortal& p) {
        const glm::dmat3 R(glm::dvec3(p.right),
                           glm::dvec3(p.upDir),
                           glm::dvec3(p.normal));
        const glm::dmat3 Rt = glm::transpose(R);
        const glm::dvec3 t  = -(Rt * p.origin);
        glm::dmat4 m(1.0);
        m[0] = glm::dvec4(Rt[0], 0.0);
        m[1] = glm::dvec4(Rt[1], 0.0);
        m[2] = glm::dvec4(Rt[2], 0.0);
        m[3] = glm::dvec4(t,     1.0);
        return m;
    }

    glm::dmat4 SrcToDst(const Client::ClientPortal& src,
                        const Client::ClientPortal& dst) {
        // Mirror = 180° rotation around portal-local Y. Flips local X
        // (right) and local Z (normal) while keeping Y (up). Same matrix
        // PortalRegistry uses server-side for the teleport math.
        glm::dmat4 mirror(1.0);
        mirror[0][0] = -1.0;
        mirror[2][2] = -1.0;
        return PortalToWorld(dst) * mirror * WorldToPortal(src);
    }

    namespace {
        // Branchless sign with the convention sign(0) = 1 (Lengyel's text
        // explicitly uses this convention so the q vector picks the +1
        // corner of the eye-space frustum when the plane is axis-aligned).
        float Sign(float v) { return (v < 0.0f) ? -1.0f : 1.0f; }
    } // namespace

    glm::mat4 ObliqueProjection(const glm::mat4& projection,
                                const glm::mat4& virtualView,
                                const Client::ClientPortal& dstPortal) {
        // ── 1. Build the clip plane in EYE space ─────────────────────────
        // World plane: outward normal = +dst.normal (points into the room
        // the player wants to see); contains the portal origin.
        // worldPlane = (n.x, n.y, n.z, -dot(n, p)).
        const glm::vec3 nWorld = glm::normalize(dstPortal.normal);
        const glm::vec3 pWorld = glm::vec3(dstPortal.origin);
        const glm::vec4 worldPlane(nWorld, -glm::dot(nWorld, pWorld));

        // To eye space: P_eye = transpose(inverse(view)) · P_world.
        // (Plane equations transform by the inverse-transpose of the same
        // matrix used to transform points.)
        const glm::mat4 invViewT = glm::transpose(glm::inverse(virtualView));
        glm::vec4 c = invViewT * worldPlane;

        // The portal renderer always wants the camera looking AT the scene
        // through the plane (positive side = visible side). If the eye lies
        // on the negative side of the plane, flip so the math works out
        // (otherwise everything visible gets clipped).
        if (c.w > 0.0f) c = -c;

        // ── 2. Lengyel's oblique projection ──────────────────────────────
        // Pick the eye-space corner Q of the near plane to which the new
        // near plane is anchored. q.xy uses the sign of c.xy so we move
        // the corner OPPOSITE the clip plane direction; q.z = -1 (eye-
        // space looks down -Z); q.w = (1 + P[2][2]) / P[3][2] derived from
        // the original projection's near-plane formulation.
        // GLM matrices are column-major: P[col][row].
        glm::vec4 q;
        q.x = (Sign(c.x) + projection[2][0]) / projection[0][0];
        q.y = (Sign(c.y) + projection[2][1]) / projection[1][1];
        q.z = -1.0f;
        q.w = (1.0f + projection[2][2]) / projection[3][2];

        // Scale clip plane so it lands exactly on the eye-space corner.
        const glm::vec4 cScaled = c * (2.0f / glm::dot(c, q));

        // Replace the third row of the projection matrix (so the projected
        // z value evaluates to -1 — the near plane in NDC — exactly when
        // a vertex lies on the clip plane). Other rows untouched.
        glm::mat4 oblique = projection;
        oblique[0][2] = cScaled.x;
        oblique[1][2] = cScaled.y;
        oblique[2][2] = cScaled.z + 1.0f;
        oblique[3][2] = cScaled.w;
        return oblique;
    }

    glm::mat4 ObliqueProjectionOriented(const glm::mat4& projection,
                                        const glm::mat4& view,
                                        const glm::vec3& planeNormal,
                                        const glm::vec3& planePoint) {
        // Same as ObliqueProjection, MINUS the auto-flip step. The plane
        // is taken as-is — the caller is responsible for orienting it
        // (normal pointing into the half-space they want to keep).
        const glm::vec3 nWorld = glm::normalize(planeNormal);
        const glm::vec4 worldPlane(nWorld, -glm::dot(nWorld, planePoint));
        const glm::mat4 invViewT = glm::transpose(glm::inverse(view));
        glm::vec4 c = invViewT * worldPlane;

        glm::vec4 q;
        q.x = (Sign(c.x) + projection[2][0]) / projection[0][0];
        q.y = (Sign(c.y) + projection[2][1]) / projection[1][1];
        q.z = -1.0f;
        q.w = (1.0f + projection[2][2]) / projection[3][2];

        const glm::vec4 cScaled = c * (2.0f / glm::dot(c, q));

        glm::mat4 oblique = projection;
        oblique[0][2] = cScaled.x;
        oblique[1][2] = cScaled.y;
        oblique[2][2] = cScaled.z + 1.0f;
        oblique[3][2] = cScaled.w;
        return oblique;
    }

    Camera ComputeVirtualCamera(const Camera& orig,
                                const Client::ClientPortal& src,
                                const Client::ClientPortal& dst) {
        const glm::dmat4 M = SrcToDst(src, dst);

        // Transform the player's eye point.
        const glm::dvec4 origPos4(static_cast<double>(orig.position.x),
                                  static_cast<double>(orig.position.y),
                                  static_cast<double>(orig.position.z),
                                  1.0);
        const glm::dvec3 newPos = glm::dvec3(M * origPos4);

        // Player forward in Camera convention (matches Camera::GetForward).
        const float yawRad   = glm::radians(orig.yaw);
        const float pitchRad = glm::radians(orig.pitch);
        const glm::dvec3 origFwd = glm::normalize(glm::dvec3(
            std::cos(yawRad) * std::cos(pitchRad),
            std::sin(pitchRad),
            std::sin(yawRad) * std::cos(pitchRad)));
        // Player's true camera up that lookAt would compute (cross of
        // right and forward, where right = cross(forward, world-Y)).
        const glm::dvec3 worldY(0.0, 1.0, 0.0);
        const glm::dvec3 origRight = glm::normalize(glm::cross(origFwd, worldY));
        const glm::dvec3 origUp    = glm::normalize(glm::cross(origRight, origFwd));

        const glm::dmat3 Mrot = glm::dmat3(M);
        const glm::dvec3 newFwd = glm::normalize(Mrot * origFwd);
        const glm::dvec3 newUp  = glm::normalize(Mrot * origUp);

        // Recover yaw/pitch from the rotated forward in Camera convention:
        //   pitch = asin(fwd.y)
        //   yaw   = atan2(fwd.z, fwd.x)
        // Clamp pitch just shy of ±90° so glm::lookAt's cross with worldUp
        // (used in Camera::GetViewMatrix) doesn't degenerate.
        constexpr float kPitchClampDeg = 89.5f;
        float newPitchDeg = glm::degrees(std::asin(
            std::clamp<double>(newFwd.y, -1.0, 1.0)));
        newPitchDeg = std::clamp(newPitchDeg, -kPitchClampDeg, kPitchClampDeg);
        const float newYawDeg = glm::degrees(std::atan2(newFwd.z, newFwd.x));

        // Roll: signed angle between the (yaw,pitch)-derived "default up"
        // and the M-rotated "true up", measured around newFwd. Non-zero
        // when source and destination portal orientations don't agree on
        // world up (floor↔wall, ceiling↔wall). For wall↔wall pairs this
        // evaluates to ~0 and the existing yaw/pitch view is unaffected.
        const float npRad = glm::radians(newPitchDeg);
        const float nyRad = glm::radians(newYawDeg);
        const glm::dvec3 derivedFwd(
            std::cos(nyRad) * std::cos(npRad),
            std::sin(npRad),
            std::sin(nyRad) * std::cos(npRad));
        const glm::dvec3 derivedRight =
            glm::normalize(glm::cross(derivedFwd, worldY));
        const glm::dvec3 derivedUp =
            glm::normalize(glm::cross(derivedRight, derivedFwd));
        // Project newUp onto plane perpendicular to derivedFwd.
        glm::dvec3 newUpProj = newUp - derivedFwd * glm::dot(newUp, derivedFwd);
        const double upLen = glm::length(newUpProj);
        float rollDeg = 0.0f;
        if (upLen > 1.0e-6) {
            newUpProj /= upLen;
            const double c = glm::dot(derivedUp,    newUpProj);
            const double s = glm::dot(derivedRight, newUpProj);
            rollDeg = glm::degrees(static_cast<float>(std::atan2(s, c)));
        }
        // Degenerate case: when newFwd is near-vertical, the
        // (yaw,pitch)-derived "default up" arbitrarily picks one of the
        // two horizontal directions perpendicular to forward. If the
        // M-rotated true up happens to be the OPPOSITE one, roll lands
        // near ±180° — which would render the see-through view upside
        // down. The portal-pair transform's "tipped over" orientation
        // is technically correct, but the visible result looks like
        // the destination world rendered inverted (floor on top, sky
        // on bottom) and reads as "underground." Clamp to 0 so the
        // view stays upright. The slight orientation mismatch between
        // see-through and post-teleport is invisible since after
        // teleport the floor/ceiling exit override snaps pitch to ±89.5°
        // anyway.
        if (std::abs(rollDeg) > 90.0f) {
            rollDeg = 0.0f;
        }

        Camera virt = orig;                     // inherit fov etc.
        virt.position = glm::vec3(newPos);
        virt.yaw      = newYawDeg;
        virt.pitch    = newPitchDeg;
        virt.roll     = rollDeg;

        // CRITICAL: compose the view matrix directly from M, bypassing the
        // yaw/pitch/roll path. The lookAt-from-yaw/pitch path is unstable
        // when the M-rotated forward is near-vertical (which happens for
        // any floor/ceiling portal in the pair, and for wall portals when
        // looking near-vertically): atan2(near_zero, near_zero) swings
        // wildly with tiny camera movements, so the see-through content
        // "floats" or "scrolls" inside the silhouette as the player
        // rotates. Computing the view matrix directly from M is stable
        // across all orientations.
        //
        // Derivation: if a world point P_dst on the dst side is visible
        // through the portal, the player should see it as if the original
        // camera were viewing P_src = M^-1 · P_dst. So:
        //     V_virt(P_dst) = V_orig(M^-1 · P_dst) = V_orig · M^-1 · P_dst
        // → V_virt = V_orig · M^-1.
        const glm::mat4 origView = orig.GetViewMatrix();
        virt.viewOverride    = origView * glm::mat4(glm::inverse(M));
        virt.hasViewOverride = true;
        return virt;
    }

} // namespace Render::PortalTransform

#endif // ENABLE_PORTAL_GUN
