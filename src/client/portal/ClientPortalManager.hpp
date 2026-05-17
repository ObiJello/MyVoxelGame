// File: src/client/portal/ClientPortalManager.hpp
//
// Client-side mirror of the server's PortalRegistry. Receives PortalSetS2C /
// PortalRemoveS2C packets via ClientPacketHandler and exposes the current set
// of active portals to the renderer (PortalRenderer in Phase 4, the stencil
// see-through passes in Phase 5+).
//
// Stays intentionally dumb: it stores only what the server tells it. There is
// no client-side prediction, no validation, no teleport math here. Every
// authoritative decision (placement validity, teleport firing, removal) lives
// on the server; the client just paints what it's told.
//
// File entirely gated on ENABLE_PORTAL_GUN — when the feature is off, this
// translation unit compiles to nothing.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include <cstdint>
#include <unordered_map>
#include <glm/glm.hpp>

namespace Game { struct AABB; }

namespace Network {
    struct PortalSetS2CPacket;
    struct PortalRemoveS2CPacket;
    struct PortalTeleportFlashS2CPacket;
    struct PortalFizzleS2CPacket;
}

namespace Client {

    // A single portal as received over the wire. Mirrors the server's `Portal`
    // struct but lives in client land so the renderer doesn't have to reach
    // into server headers. `right` is computed lazily from `cross(upDir,
    // normal)` (the wire format omits it to save 12 bytes per packet).
    struct ClientPortal {
        glm::dvec3 origin{0.0};
        glm::vec3  normal{0.0f, 0.0f, 1.0f};
        glm::vec3  upDir {0.0f, 1.0f, 0.0f};
        glm::vec3  right {1.0f, 0.0f, 0.0f};   // recomputed on Set
        bool       active = false;

        // Portal-authoritative animation state. Mirrors c_prop_portal.cpp's
        // m_fOpenAmount and m_fStaticAmount (see ClientThink at line 219).
        //
        // openStartTimeSec : wall-clock time when this portal was placed.
        //   OpenAmount(now) ramps 0 → 1 over kOpenDurationSec (Portal: 0.5s).
        //   Drives the refraction sub-pass (only fires while opening).
        //
        // staticPingStartTimeSec : wall-clock time when this portal was
        //   "pinged" by its pair-mate being fired. StaticAmount(now) decays
        //   1 → 0 over kStaticDurationSec (Portal: 1.0s). When non-zero,
        //   the see-through view is veiled by the static overlay.
        double openStartTimeSec       = 0.0;
        double staticPingStartTimeSec = -1.0e9; // far past = no ping
    };

    // Portal-authoritative animation durations from c_prop_portal.cpp:241,224.
    constexpr double kOpenDurationSec   = 0.5;  // m_fOpenAmount  += dt * 2.0
    constexpr double kStaticDurationSec = 1.0;  // m_fStaticAmount -= dt

    struct ClientPortalPair {
        ClientPortal blue;
        ClientPortal orange;

        // Teleport flash — both portals light up briefly when something
        // teleports through the pair. flashEndTimeSec is wall-clock
        // glfwGetTime() at which the flash expires; the renderer
        // computes the per-frame intensity by subtracting now() and
        // dividing by kFlashDurationSec. Zero = no flash active.
        double flashEndTimeSec = 0.0;
    };

    // Total flash duration in seconds. Matches Portal-game's brief light-up
    // (~0.35s feels right for the timing of a teleport "punctuation").
    constexpr double kFlashDurationSec = 0.35;

    class ClientPortalManager {
    public:
        // Apply a server PortalSet packet. Replaces the matching color on the
        // matching gun (auto-creates the pair if first time we hear about it).
        void OnPortalSet(const Network::PortalSetS2CPacket& packet);

        // Apply a server PortalRemove packet.
        //   color = 0 → erase blue only
        //   color = 1 → erase orange only
        //   color = 2 → erase whole pair (the shift-clear gesture from Phase 2)
        void OnPortalRemove(const Network::PortalRemoveS2CPacket& packet);

        // Apply a server TeleportFlash packet — set the pair's
        // flashEndTimeSec to (now + kFlashDurationSec) so the renderer
        // shows the brief light-up.
        void OnTeleportFlash(const Network::PortalTeleportFlashS2CPacket& packet);

        // Apply a server PortalFizzle packet — dispatches a one-shot
        // particle burst via PortalParticleSystem::EmitOneShot.
        // No client state changes; purely a visual hook.
        void OnPortalFizzle(const Network::PortalFizzleS2CPacket& packet);

        // Drop everything (e.g. on disconnect / world reload).
        void Clear() { m_pairs.clear(); }

        // Read-only iteration for the renderer. Callback signature:
        //   void(uint64_t gunId, const ClientPortalPair& pair)
        template<typename F>
        void ForEachPair(F&& callback) const {
            for (const auto& [gunId, pair] : m_pairs) callback(gunId, pair);
        }

        size_t PairCount() const { return m_pairs.size(); }

        // True iff (x,y,z) names one of the two wall blocks behind any
        // FULLY-PAIRED portal AND the player AABB fits LATERALLY inside
        // the 1×2 portal opening (in the portal's tangent plane). The
        // player physics collision hook (Physics.cpp's
        // PortalPassthroughFn) calls this — when true, the block is
        // treated as non-solid for that specific player position.
        //
        // The lateral-fit check makes the passthrough DIRECTIONAL:
        //   • Player approaching the front face along the portal normal
        //     has AABB extent that fits inside the opening rectangle →
        //     true → walk through.
        //   • Player approaching from the side has AABB extent that
        //     exceeds the opening in a tangent axis → false → wall stays
        //     solid; can't tunnel through the block sideways.
        //
        // Gating on "both portals active" matters: if only one portal
        // is placed, the wall stays solid for everyone — otherwise the
        // player would walk INTO the wall and get stuck (no destination
        // to teleport to).
        bool IsBlockBehindActivePortal(int x, int y, int z,
                                       const Game::AABB& playerAABB) const;

        // Ghost-rendering query (#4 — half-body across portal). Tests
        // whether the player whose feet are at `playerPos` is currently
        // straddling any active portal pair: AABB center close enough to
        // the source plane to cross it AND laterally inside the 1×2
        // oval. If yes, returns the source→destination transform M and
        // the destination portal's plane equation so the renderer can:
        //   • Draw a ghost copy of the player transformed by M.
        //   • Clip the ghost so only the "emerged" half (on +dstNormal
        //     side of the destination plane) is visible.
        // valid = false → not straddling, skip the ghost render.
        struct GhostInfo {
            bool      valid = false;
            glm::mat4 transform{1.0f};
            glm::vec4 exitClipPlane{0.0f};   // (n.xyz, -dot(n, dstOrigin))
            glm::vec4 entryClipPlane{0.0f};  // (n.xyz, -dot(n, srcOrigin))
        };
        GhostInfo GetStraddlingGhost(const glm::vec3& playerPos,
                                     float playerHeight) const;

        // Client-side teleport prediction (#3 follow-up). The server
        // teleport runs at 20 TPS, so the player can spend up to ~50 ms
        // (≈ 22 cm at walking speed) past the source portal plane before
        // the server-acknowledged teleport arrives. During that window
        // the portal mesh is BEHIND the local camera and stops drawing,
        // so the player sees the wall geometry the camera is now inside
        // of. Predicting the teleport client-side as soon as the eye
        // crosses eliminates this gap — the camera never spends frames
        // past the source plane.
        //
        // Inputs:
        //   prevEye / currEye   eye position (= feet + EYE_HEIGHT) at the
        //                        previous frame and the current frame.
        //   currFeet            current feet position (the player's
        //                        physics origin, used to compute the
        //                        post-teleport feet position).
        //   currVel             current player velocity (will be rotated
        //                        through the portal-pair matrix).
        //   currYawDeg          current camera yaw in degrees (will be
        //                        rotated through the portal-pair matrix).
        //   playerHeight        player AABB height (1.8 m standing, etc.)
        //   playerHalfWidth     player AABB half-width (0.3 m).
        //
        // Returns valid=true if a crossing was detected this frame; the
        // caller should immediately overwrite physics.position, .velocity
        // and camera.yaw with the returned values. Server's teleport
        // packet (when it eventually arrives) will be a no-op since
        // both sides compute the same destination.
        struct TeleportPrediction {
            bool      valid = false;
            glm::vec3 newFeet{0.0f};
            glm::vec3 newVelocity{0.0f};
            float     newYawDeg   = 0.0f;
            float     newPitchDeg = 0.0f;
        };
        TeleportPrediction CheckEyeCrossing(const glm::vec3& prevEye,
                                            const glm::vec3& currEye,
                                            const glm::vec3& currFeet,
                                            const glm::vec3& currVel,
                                            float currYawDeg,
                                            float currPitchDeg,
                                            float playerHeight,
                                            float playerHalfWidth) const;

    private:
        std::unordered_map<uint64_t, ClientPortalPair> m_pairs;
    };

    // Process-global instance. Consumed by the packet handler (writer) and
    // PortalRenderer (reader). Single-threaded — only touched on the render /
    // packet thread.
    ClientPortalManager& GetClientPortalManager();

} // namespace Client

#endif // ENABLE_PORTAL_GUN
