// File: src/client/renderer/portal/PortalCameraTransform.hpp
//
// Math helpers for the see-through portal pass (Phase 6+). Computes a
// "virtual" Render::Camera that sees the world from the destination portal's
// vantage point — the player looks at the source portal, but what they SEE
// in that region of the screen is the destination side of the world.
//
// The transform is the Portal-classic mirror:
//   M = T_dst · Mirror180Up · inverse(T_src)
// where T_src / T_dst are the orthonormal local→world matrices of the source
// and destination portals (basis = right, upDir, normal; translation = origin).
//
// File entirely gated on ENABLE_PORTAL_GUN.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "../core/Camera.hpp"
#include "client/portal/ClientPortalManager.hpp"
#include <glm/glm.hpp>

namespace Render {

    namespace PortalTransform {

        // Build the source portal's local→world matrix. Columns:
        //   [right | upDir | normal | origin].
        // Used by both the mirror math and the depth-refill geometry.
        glm::dmat4 PortalToWorld(const Client::ClientPortal& p);

        // Inverse of an orthonormal-basis matrix = transpose of rotation
        // plus negated translation. Cheaper and more numerically stable
        // than glm::inverse() — the basis is unit-orthogonal by construction.
        glm::dmat4 WorldToPortal(const Client::ClientPortal& p);

        // Full source→destination world transform.
        glm::dmat4 SrcToDst(const Client::ClientPortal& src,
                            const Client::ClientPortal& dst);

        // Build the virtual camera that sees the world from the destination
        // side. Position is M·orig.position; look direction is mat3(M)·forward
        // (where forward derives from orig's yaw/pitch). The virtual camera's
        // yaw/pitch are recovered from that direction so the existing
        // Camera::GetViewMatrix() (and the chunk-renderer's downstream
        // occlusion graph that reads yaw/pitch) Just Works.
        //
        // PITCH CLAMP: pitch is clamped to ±89.5° to keep the lookAt math
        // (which uses a fixed world-up) from collapsing into a degenerate
        // basis when the player stares straight down a floor portal that
        // pairs to a ceiling portal. The visual consequence is a tiny
        // (≤0.5°) tilt at the extreme — not noticeable in practice and far
        // preferable to the camera popping to garbage on degenerate frames.
        Camera ComputeVirtualCamera(const Camera& orig,
                                    const Client::ClientPortal& src,
                                    const Client::ClientPortal& dst);

        // Modify a perspective projection so its near clipping plane is the
        // destination portal's surface. Implements Eric Lengyel's "Modifying
        // the Projection Matrix to Perform Oblique Near-plane Clipping"
        // (Game Programming Gems 5, 2005).
        //
        // Why: M = T_dst·Mirror180Up·inverse(T_src) puts the virtual camera
        // on the BEHIND side of the destination portal's wall — that's the
        // correct mirror geometry, but the wall block then occludes the
        // destination room from the virtual camera's view. The oblique near
        // plane clips every fragment whose eye-space depth is on the wall
        // side of the portal plane, leaving only the room visible.
        //
        // `dstPortal` is the destination portal; `virtualView` is the
        // virtual camera's view matrix (the same V you pass to chunk
        // render). The plane's outward direction is `+dstPortal.normal`
        // (pointing into the room the player wants to see), and the plane
        // is positioned at the portal's origin.
        glm::mat4 ObliqueProjection(const glm::mat4& projection,
                                    const glm::mat4& virtualView,
                                    const Client::ClientPortal& dstPortal);

        // Same as ObliqueProjection but WITHOUT the safety flip that
        // forces the kept side to match the camera's side. The caller
        // guarantees the plane's normal points toward the kept side and
        // accepts whatever Lengyel's math produces if the camera ends
        // up on either side.
        //
        // Used by the split-screen close-up render (camera straddling
        // the portal plane while looking sideways): we MUST keep a
        // specific world half-space (the source room for the main cam,
        // the destination room for the virt cam) regardless of which
        // side of the plane the camera happens to be on.
        glm::mat4 ObliqueProjectionOriented(const glm::mat4& projection,
                                            const glm::mat4& view,
                                            const glm::vec3& planeNormal,
                                            const glm::vec3& planePoint);

    } // namespace PortalTransform

} // namespace Render

#endif // ENABLE_PORTAL_GUN
