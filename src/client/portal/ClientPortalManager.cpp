// File: src/client/portal/ClientPortalManager.cpp
// See ClientPortalManager.hpp for scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "ClientPortalManager.hpp"

#include "common/network/packets/game/PortalSetS2CPacket.hpp"
#include "common/network/packets/game/PortalRemoveS2CPacket.hpp"
#include "common/network/packets/game/PortalTeleportFlashS2CPacket.hpp"
#include "common/network/packets/game/PortalFizzleS2CPacket.hpp"
#include "common/core/Log.hpp"
#include "common/physics/Physics.hpp"  // Game::AABB
#include "client/renderer/portal/PortalParticleSystem.hpp"
#include "client/renderer/portal/PortalCrosshair.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>  // std::clamp — required for MSVC, transitively
                      // included on libc++/libstdc++ but not on MSVC's STL

namespace Client {

    ClientPortalManager& GetClientPortalManager() {
        static ClientPortalManager instance;
        return instance;
    }

    void ClientPortalManager::OnPortalSet(const Network::PortalSetS2CPacket& p) {
        ClientPortal portal;
        portal.origin = glm::dvec3(p.originX, p.originY, p.originZ);
        portal.normal = glm::vec3(p.normalX, p.normalY, p.normalZ);
        portal.upDir  = glm::vec3(p.upX,     p.upY,     p.upZ);
        // Recover `right` from the basis. Server guarantees normal and upDir
        // are unit + perpendicular, so the cross is a unit vector — no
        // normalize needed (and we want to skip the divide on the hot path).
        portal.right  = glm::cross(portal.upDir, portal.normal);
        portal.active = true;

        // Portal-authoritative timings (c_prop_portal.cpp:587-595): the new
        // portal opens from 0; if the OTHER color of the pair is already
        // active, it gets a "static ping" that fades over kStaticDurationSec
        // — Portal's signature visual when a portal is fired with its
        // partner already on the wall.
        const double now    = glfwGetTime();
        portal.openStartTimeSec       = now;
        portal.staticPingStartTimeSec = -1.0e9;

        ClientPortalPair& pair = m_pairs[p.gunId];
        if (p.color == 0) {
            pair.blue = portal;
            if (pair.orange.active) pair.orange.staticPingStartTimeSec = now;
        } else {
            pair.orange = portal;
            if (pair.blue.active)   pair.blue.staticPingStartTimeSec   = now;
        }

        Log::Info("[ClientPortal] Set %s portal for gun=%llu at "
                  "(%.2f,%.2f,%.2f) normal=(%.0f,%.0f,%.0f)",
                  p.color == 0 ? "BLUE" : "ORANGE",
                  static_cast<unsigned long long>(p.gunId),
                  p.originX, p.originY, p.originZ,
                  p.normalX, p.normalY, p.normalZ);
        // Flip the bracket on that side from outline to filled.
        Render::PortalCrosshair::NotifyPortalPlaced(p.color);
    }

    void ClientPortalManager::OnPortalRemove(const Network::PortalRemoveS2CPacket& p) {
        // Revert the bracket on the cleared side(s) back to outline.
        // Done up-front so the swap happens even when we early-return
        // on color==2 below.
        Render::PortalCrosshair::NotifyPortalRemoved(p.color);

        auto it = m_pairs.find(p.gunId);
        if (it == m_pairs.end()) return;

        if (p.color == 2) {
            // Whole pair — drop the entry entirely.
            m_pairs.erase(it);
            Log::Info("[ClientPortal] Cleared pair for gun=%llu",
                      static_cast<unsigned long long>(p.gunId));
            return;
        }

        if (p.color == 0) it->second.blue   = ClientPortal{};
        else              it->second.orange = ClientPortal{};

        // Garbage-collect a pair whose last portal just went away.
        if (!it->second.blue.active && !it->second.orange.active) {
            m_pairs.erase(it);
        }
    }

    bool ClientPortalManager::IsBlockBehindActivePortal(int x, int y, int z,
                                                        const Game::AABB& playerAABB) const {
        // Wall block coordinates recovered from the portal's pose (see
        // PortalRegistry.cpp::MakeCandidate for the inverse construction):
        //   wallMidpoint = origin - 0.5*normal
        //   wallA/B      = floor(wallMidpoint ∓ 0.5*upDir)
        //
        // We ALSO consider blocks one step deeper in -normal direction
        // (wallA - normal, wallB - normal). This "tunnel extension"
        // is needed so the eye-based trigger (fires when the eye
        // reaches the plane → body 1.62 m past the plane) can be
        // reached physically without the player AABB colliding with
        // the solid block immediately behind the portal block.
        for (const auto& [gunId, pair] : m_pairs) {
            if (!(pair.blue.active && pair.orange.active)) continue;
            const ClientPortal* sides[2] = { &pair.blue, &pair.orange };
            for (const ClientPortal* p : sides) {
                const glm::dvec3 wallMid =
                    p->origin - 0.5 * glm::dvec3(p->normal);
                const glm::dvec3 halfUp = 0.5 * glm::dvec3(p->upDir);
                const glm::ivec3 wallA = glm::ivec3(glm::floor(wallMid - halfUp));
                const glm::ivec3 wallB = glm::ivec3(glm::floor(wallMid + halfUp));
                // Tunnel extension: 1 block deeper in -normal direction.
                const glm::ivec3 normalI = glm::ivec3(glm::round(p->normal));
                const glm::ivec3 wallAdeep = wallA - normalI;
                const glm::ivec3 wallBdeep = wallB - normalI;
                const bool isWallBlock =
                    (x == wallA.x     && y == wallA.y     && z == wallA.z) ||
                    (x == wallB.x     && y == wallB.y     && z == wallB.z) ||
                    (x == wallAdeep.x && y == wallAdeep.y && z == wallAdeep.z) ||
                    (x == wallBdeep.x && y == wallBdeep.y && z == wallBdeep.z);
                if (!isWallBlock) continue;

                // Lateral-fit test: project the player AABB onto the
                // portal's tangent axes (right, upDir) and check that
                // the projected interval lies entirely inside the 1×2
                // opening rectangle ([-0.5, +0.5] × [-1.0, +1.0] in
                // local plane coords). If the AABB pokes outside the
                // rectangle along either tangent axis, the player is
                // partially over the wall material around the opening
                // and the block stays solid for them.
                //
                // Support function for an axis-aligned box on an
                // arbitrary unit axis: half-extent on axis = Σ |axis_i|·
                // halfExtent_i. Center distance = dot(center - origin,
                // axis). Interval = [center - half, center + half].
                const glm::vec3 center = (playerAABB.min + playerAABB.max) * 0.5f;
                const glm::vec3 half   = (playerAABB.max - playerAABB.min) * 0.5f;
                const glm::vec3 dCenter = center - glm::vec3(p->origin);

                auto fitsAxis = [&](const glm::vec3& axis, float halfExtent) {
                    const float c = glm::dot(dCenter, axis);
                    const float h = std::abs(axis.x) * half.x +
                                    std::abs(axis.y) * half.y +
                                    std::abs(axis.z) * half.z;
                    return (c - h >= -halfExtent) && (c + h <= halfExtent);
                };

                if (!fitsAxis(p->right, 0.5f))  continue;
                if (!fitsAxis(p->upDir, 1.0f))  continue;
                return true;
            }
        }
        return false;
    }

    namespace {
        // Build the world-space basis of a portal: columns = right, up,
        // normal, translation = origin. Mirrors PortalRegistry's
        // PortalToWorld / WorldToPortal pair so the resulting M matrix
        // equals the server's SrcToDst transform exactly (and the camera
        // virtual-pose used by PortalCameraTransform). Float precision is
        // sufficient — players are within ±10⁵ m of origin, far below
        // float epsilon for visible artifacts on this scale.
        glm::mat4 PortalToWorldF(const ClientPortal& p) {
            return glm::mat4{
                glm::vec4(p.right,  0.0f),
                glm::vec4(p.upDir,  0.0f),
                glm::vec4(p.normal, 0.0f),
                glm::vec4(glm::vec3(p.origin), 1.0f),
            };
        }
        glm::mat4 SrcToDstF(const ClientPortal& src, const ClientPortal& dst) {
            const glm::mat4 srcM = PortalToWorldF(src);
            const glm::mat4 dstM = PortalToWorldF(dst);
            glm::mat4 mirror(1.0f);
            mirror[0][0] = -1.0f;  // flip right
            mirror[2][2] = -1.0f;  // flip normal
            return dstM * mirror * glm::inverse(srcM);
        }
    } // namespace

    ClientPortalManager::GhostInfo
    ClientPortalManager::GetStraddlingGhost(const glm::vec3& playerPos,
                                            float playerHeight) const {
        // Body center used for both the plane-distance and lateral tests.
        const glm::vec3 center = playerPos +
            glm::vec3(0.0f, playerHeight * 0.5f, 0.0f);

        // 1.0 m straddle window — picked to cover both the player's body
        // half-width along a wall (~0.3 m) and half-height along a
        // floor/ceiling (~0.9 m), with margin so the ghost appears
        // slightly before the body fully reaches the plane.
        constexpr float kStraddleNormalRange = 1.0f;
        constexpr float kStraddleLateralS    = 0.7f;  // half-width 0.5 + 0.2 m margin
        constexpr float kStraddleLateralT    = 1.2f;  // half-height 1.0 + 0.2 m margin

        for (const auto& [gunId, pair] : m_pairs) {
            if (!(pair.blue.active && pair.orange.active)) continue;
            struct Side { const ClientPortal* src; const ClientPortal* dst; };
            const Side sides[2] = {
                { &pair.blue,   &pair.orange },
                { &pair.orange, &pair.blue   },
            };
            for (const Side& s : sides) {
                const glm::vec3 d = center - glm::vec3(s.src->origin);
                const float sd = glm::dot(d, s.src->normal);
                if (std::abs(sd) > kStraddleNormalRange) continue;

                const float su = glm::dot(d, s.src->right);
                const float tu = glm::dot(d, s.src->upDir);
                if (std::abs(su) > kStraddleLateralS) continue;
                if (std::abs(tu) > kStraddleLateralT) continue;

                GhostInfo g;
                g.valid     = true;
                g.transform = SrcToDstF(*s.src, *s.dst);
                // Plane keeping the +normal side: discard fragments where
                // dot(p, n) + w < 0  ⇔  dot(p - origin, n) < 0.
                const glm::vec3 dstO = glm::vec3(s.dst->origin);
                g.exitClipPlane  = glm::vec4(s.dst->normal,
                                             -glm::dot(s.dst->normal, dstO));
                const glm::vec3 srcO = glm::vec3(s.src->origin);
                g.entryClipPlane = glm::vec4(s.src->normal,
                                             -glm::dot(s.src->normal, srcO));
                return g;
            }
        }
        return GhostInfo{};
    }

    ClientPortalManager::TeleportPrediction
    ClientPortalManager::CheckEyeCrossing(const glm::vec3& prevEye,
                                          const glm::vec3& currEye,
                                          const glm::vec3& currFeet,
                                          const glm::vec3& currVel,
                                          float currYawDeg,
                                          float currPitchDeg,
                                          float playerHeight,
                                          float /*playerHalfWidth*/) const {
        // EYE-based trigger (Portal-faithful FEEL — Portal triggers
        // body teleport at center crossing in source, but FORCES the
        // player to duck during cross-orientation teleports
        // (prop_portal.cpp:1037 ForceDuckThisFrame), shrinking the
        // body so eye-to-center is only ~25 cm — the trigger feels
        // like "eye at plane" because the eye is so close to center.
        //
        // We don't have ducking, so eye-to-waist is 0.72 m at standing
        // height. Triggering on waist crossing means the eye is still
        // 0.72 m above the source plane when teleport fires — the
        // player sees their character "pop through" the floor instead
        // of "fall fully into" it.
        //
        // Switching to eye-based crossing matches the perceived Portal
        // behaviour: trigger fires when the player's eye reaches the
        // source plane, body fully past plane.

        // Trigger distance — tuned by feel.
        constexpr float kEarlyPredictDistance = 0.085f;

        for (const auto& [gunId, pair] : m_pairs) {
            if (!(pair.blue.active && pair.orange.active)) continue;
            struct Side { const ClientPortal* src; const ClientPortal* dst; };
            const Side sides[2] = {
                { &pair.blue,   &pair.orange },
                { &pair.orange, &pair.blue   },
            };
            for (const Side& s : sides) {
                const glm::vec3 nrm = s.src->normal;
                const glm::vec3 origin = glm::vec3(s.src->origin);

                // Eye crossing check — fire when the player approached
                // from the +normal side and is now within (or past)
                // the trigger window. Using prevEye→currEye instead
                // of a symmetric ±window means:
                //   • Fast falls that overshoot in one frame (curr
                //     deep negative) still trigger, because prev was
                //     positive — fixes "stuck under floor portal".
                //   • Players sitting behind the wall (prev ≤ 0) never
                //     trigger, regardless of distance — fixes the
                //     "20 blocks behind the wall" warp bug.
                //   • Front-only entry naturally falls out.
                const float currSigned = glm::dot(currEye - origin, nrm);
                const float prevSigned = glm::dot(prevEye - origin, nrm);

                if (currSigned > kEarlyPredictDistance) continue;
                if (prevSigned <= 0.0f) continue;

                // Direction-of-motion check using POSITION DELTA, not
                // velocity. The client's player.physics.velocity can be
                // stale (e.g. left over from a previous predicted
                // teleport's exit velocity) while the actual per-frame
                // eye motion is in the opposite direction. Reading the
                // position delta directly from prevEye→currEye is the
                // ground truth for motion direction. If currSigned >
                // prevSigned (eye moving in +normal direction = away
                // from plane), skip the trigger.
                if (currSigned > prevSigned + 1.0e-4f) continue;

                // Lateral fit using current eye position.
                const glm::vec3 d = currEye - origin;
                const float su = glm::dot(d, s.src->right);
                const float tu = glm::dot(d, s.src->upDir);
                if (std::abs(su) > 0.5f || std::abs(tu) > 1.0f) continue;

                // Position transform: take the EYE through M, then
                // derive feet from new eye by subtracting eye height
                // along world Y. This places the post-teleport eye
                // exactly at the position M would map the source eye
                // to — Portal's "eye high to the new portal's plane"
                // result for cross-orientation.
                const float eyeHeight = currEye.y - currFeet.y;
                const glm::mat4 M = SrcToDstF(*s.src, *s.dst);
                const glm::vec3 newEye = glm::vec3(M * glm::vec4(currEye, 1.0f));
                glm::vec3 newFeet = newEye - glm::vec3(0.0f, eyeHeight, 0.0f);
                glm::vec3 newVel  = glm::mat3(M) * currVel;


                // Y override — place EYE just past the dst plane on
                // its +normal side. This matches Portal's "eye high to
                // the new portal's plane" feel: the eye is right at
                // the destination plane post-teleport, body extends
                // into the destination room.
                //
                //   • Ceiling dst (normal.y < -0.7): eye 5cm below
                //     ceiling, body hangs down into the room.
                //   • Floor dst (normal.y > +0.7): eye 5cm above floor,
                //     body straddles floor plane (head above, feet
                //     below). Min exit velocity below pops them up.
                //   • Wall dst (|normal.y| < 0.3): preserve Y from M
                //     (vertical wall preserves Y naturally).
                // Slightly larger than kEarlyPredictDistance (0.087) so
                // the post-teleport eye is OUTSIDE the trigger zone on
                // the destination side — otherwise a stopped player at
                // the post-teleport position would satisfy
                // `curr ≤ threshold` and re-trigger forever.
                constexpr float kEyeOffsetFromPlane = 0.086f;
                const float dstNy = s.dst->normal.y;
                if (dstNy > 0.7f) {
                    // Floor: eye at floor + 5cm.
                    newFeet.y = static_cast<float>(s.dst->origin.y)
                                + kEyeOffsetFromPlane - eyeHeight;
                } else if (dstNy < -0.7f) {
                    // Ceiling: eye placed so the head doesn't poke into
                    // the ceiling block AND the eye is well outside the
                    // trigger window. Head is 0.18 m above the eye
                    // (eye height 1.62, body height 1.8). Using a 0.2 m
                    // offset puts the head 0.02 m below the ceiling
                    // plane and gives ~0.115 m of margin against the
                    // 0.085 m trigger threshold — enough to absorb FP
                    // jitter and prevent instant re-trigger.
                    constexpr float kCeilingEyeOffset = 0.1f;
                    newFeet.y = static_cast<float>(s.dst->origin.y)
                                - kCeilingEyeOffset - eyeHeight;
                } else {
                    // Vertical wall — push the eye slightly OFF the
                    // dst plane in +dst.normal direction (= into the
                    // dst room). Without this, M puts the eye 5 cm
                    // INSIDE the wall on the -dst.normal side. Push
                    // by enough that the eye lands at +kEyeOffsetFromPlane
                    // on +dst.normal side, matching the convention
                    // used for floor/ceiling exits.
                    const glm::vec3 currEyePos = newFeet + glm::vec3(0.0f, eyeHeight, 0.0f);
                    const float currEyeSd = glm::dot(
                        currEyePos - glm::vec3(s.dst->origin), s.dst->normal);
                    const float adjustment = kEyeOffsetFromPlane - currEyeSd;
                    newFeet += s.dst->normal * adjustment;

                    // Floor/ceiling-source → wall-dest puts the EYE at
                    // the wall portal's Y center, which leaves the
                    // FEET 1.62 m below — typically inside the floor
                    // block beneath the portal opening. Clamp feet to
                    // the wall portal's bottom edge so the player
                    // stands on the surface below.
                    const float portalBottomY =
                        static_cast<float>(s.dst->origin.y) - 1.0f;
                    if (newFeet.y < portalBottomY) {
                        newFeet.y = portalBottomY;
                    }
                }

                // Minimum upward exit velocity for floor portals.
                // Bumped to 12 m/s so the body fully clears the floor
                // plane (need to rise ≥ 1.62 m at gravity 32 m/s² →
                // v_min = √(2·32·1.62) ≈ 10.18 m/s; 12 leaves margin).
                if (dstNy > 0.7f) {
                    constexpr float kMinFloorExitVelocity = 12.0f;
                    if (newVel.y < kMinFloorExitVelocity) {
                        newVel.y = kMinFloorExitVelocity;
                    }
                }

                // Rotate the player's full forward vector through M and
                // extract new yaw + new pitch. Using the 3D forward
                // (not just horizontal) means the see-through view's
                // pitch and the after-teleport pitch agree — no sudden
                // tilt when the camera passes through the portal.
                //
                // When src/dst orientations differ AND the player has
                // (close to) zero pitch, the rotated forward can land
                // exactly on the world up axis. In that case the yaw
                // is genuinely degenerate; fall back to the dst
                // portal's natural exit direction so the player ends
                // up aligned with the portal's long axis.
                const float yawRad   = glm::radians(currYawDeg);
                const float pitchRad = glm::radians(currPitchDeg);
                const float cp = std::cos(pitchRad);
                const glm::vec3 fwd(
                    std::cos(yawRad) * cp,
                    std::sin(pitchRad),
                    std::sin(yawRad) * cp);
                const glm::vec3 newFwd =
                    glm::normalize(glm::mat3(M) * fwd);

                float newPitchDeg = glm::degrees(std::asin(
                    std::clamp(newFwd.y, -1.0f, 1.0f)));
                newPitchDeg = std::clamp(newPitchDeg, -89.5f, 89.5f);

                float newYawDeg;
                const float horizLen2 = newFwd.x * newFwd.x +
                                        newFwd.z * newFwd.z;
                if (horizLen2 > 0.01f) {
                    newYawDeg = glm::degrees(
                        std::atan2(newFwd.z, newFwd.x));
                } else if (std::abs(s.dst->normal.y) > 0.7f) {
                    newYawDeg = glm::degrees(std::atan2(
                        -s.dst->upDir.z, -s.dst->upDir.x));
                } else {
                    // Wall dst with degenerate horizontal forward
                    // (player looking near-straight up/down). Recover
                    // yaw from the M-rotated RIGHT vector — right is
                    // horizontal by construction regardless of pitch,
                    // so it always gives a well-defined yaw. Preserves
                    // the player's body orientation through the
                    // wall→wall mirror instead of snapping them to
                    // face out of the dst wall.
                    const glm::vec3 rightH(
                        -std::sin(yawRad), 0.0f, std::cos(yawRad));
                    const glm::vec3 newRight = glm::normalize(
                        glm::mat3(M) * rightH);
                    newYawDeg = glm::degrees(
                        std::atan2(-newRight.x, newRight.z));
                }

                // Any horizontal↔horizontal pair (floor↔floor,
                // ceiling↔ceiling, floor↔ceiling): preserve the
                // player's yaw/pitch. The player can enter a
                // floor/ceiling portal from any direction, so their
                // input facing should be kept through teleport
                // instead of being snapped to the portal's upDir.
                // (For wall-source teleports the -upDir fallback
                // still fires since you can't enter a wall portal
                // sideways — your approach direction IS the portal's
                // alignment.)
                const bool srcHoriz = std::abs(s.src->normal.y) > 0.7f;
                const bool dstHoriz = std::abs(s.dst->normal.y) > 0.7f;
                if (srcHoriz && dstHoriz) {
                    newYawDeg   = currYawDeg;
                    newPitchDeg = currPitchDeg;
                }


                TeleportPrediction p;
                p.valid       = true;
                p.newFeet     = newFeet;
                p.newVelocity = newVel;
                p.newYawDeg   = newYawDeg;
                p.newPitchDeg = newPitchDeg;
                return p;
            }
        }
        return TeleportPrediction{};
    }

    void ClientPortalManager::OnTeleportFlash(const Network::PortalTeleportFlashS2CPacket& p) {
        // Lazy-create the pair entry if it doesn't exist (shouldn't
        // normally happen — server only fires teleport on an existing
        // pair — but defensive coding for packet reordering).
        auto& pair = m_pairs[p.gunId];
        pair.flashEndTimeSec = glfwGetTime() + kFlashDurationSec;
    }

    void ClientPortalManager::OnPortalFizzle(const Network::PortalFizzleS2CPacket& p) {
        const glm::vec3 origin(static_cast<float>(p.originX),
                               static_cast<float>(p.originY),
                               static_cast<float>(p.originZ));
        const glm::vec3 normal(p.normalX, p.normalY, p.normalZ);
        const bool isOrange = (p.color == 1);
        // Reason byte mirrors PortalParticleSystem::BurstKind on the
        // server (see PortalRegistry.cpp kFizzleBadSurface/kFizzleClose).
        const auto kind = (p.reason == 1)
            ? Render::PortalParticleSystem::BurstKind::Close
            : Render::PortalParticleSystem::BurstKind::BadSurface;
        Render::g_portalParticleSystem.EmitOneShot(kind, origin, normal, isOrange);
    }

} // namespace Client

#endif // ENABLE_PORTAL_GUN
