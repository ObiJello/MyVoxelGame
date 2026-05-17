// File: src/server/portal/PortalRegistry.hpp
//
// Server-authoritative portal pair store. Each registered "portal gun"
// (keyed by a stable instance id stamped on the gun's ItemStack via the
// PORTAL_GUN_INSTANCE_ID DataComponent) owns a pair of portals — one Blue,
// one Orange. Firing a portal of a given color REPLACES that color's
// previous instance, so each pair has at most one Blue and one Orange
// portal at any time. Both must be active for stepping through one to
// teleport the player to the other.
//
// SCOPE
//   • Allocate stable per-gun ids (`AllocId`).
//   • Validate + register a placement (`PlacePortal`) — fizzles if the wall
//     surface or air clearance can't fit the 1×2 portal volume.
//   • Per-tick crossing detection (`Tick`) — when a player's position crosses
//     the portal plane within the 1×2 oval AND the paired portal is active,
//     teleport the player to the destination via ServerConnection::Teleport.
//   • Read-only access to the pair set (`All`) for Phase 3 broadcast wiring.
//
// OUT OF SCOPE FOR PHASE 2 (lands later)
//   • Network broadcast (Phase 3 — PortalSetS2C / PortalRemoveS2C packets).
//   • Velocity preservation across teleport (needs new wire field on
//     ClientboundPlayerPositionPacket; deferred to Phase 6 with the visuals).
//   • Floor / ceiling portals (the camera math is materially harder; vertical
//     walls only for Phase 2 — clicks on top/bottom faces fizzle).
//   • Persistence — pairs live for the session only; world reload clears them.
//
// THREADING
//   All operations run on the server thread. The registry is a process-global
//   singleton (`ServerRegistry()`) — no multi-world support yet because the
//   integrated server only ever holds one world.
//
// File entirely gated on ENABLE_PORTAL_GUN. When the feature is off, this
// translation unit compiles to nothing and the include of this header from
// other TUs (PortalGunBehavior.cpp, IntegratedServer.cpp) similarly skips
// every declaration here, so call-sites stripped by their own #if guard
// don't even see the type.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include <cstdint>
#include <unordered_map>
#include <glm/glm.hpp>

namespace Game {
    class World;
    struct BlockHitResult;
}
namespace Server {
    class ServerPlayer;
    class IntegratedServer;
    class ServerConnection;
}

namespace Game::Portal {

    // 0 = Blue, 1 = Orange. Wire-stable raw values — both Phase 1 (the gun's
    // PORTAL_GUN_NEXT_COLOR DataComponent) and Phase 3 (the PortalSetS2C
    // packet) treat the byte as raw. Don't reorder.
    enum class PortalColor : uint8_t { Blue = 0, Orange = 1 };

    enum class PlaceResult {
        Placed,    // Validation passed, portal registered (and replaced any prior one of the same color)
        Fizzled,   // Validation failed — caller should return UseResult::Fail and play the fizzle sound
    };

    // A single placed portal. The basis (right, upDir, normal) is an
    // orthonormal frame: `normal` faces OUT of the wall toward the player;
    // `upDir` is the portal's local +Y (always the LONG axis = 2 blocks);
    // `right` is `cross(upDir, normal)` (always the SHORT axis = 1 block).
    // The portal occupies world coordinates
    //   origin + s·right + t·upDir,   s ∈ [-0.5, +0.5], t ∈ [-1.0, +1.0].
    //
    // Orientation is encoded by CHOICE OF UPDIR, not by separate width/height
    // fields. Examples:
    //   • Vertical 1×2 on a wall: upDir = world (0,1,0).
    //   • Horizontal 2×1 on a wall (when vertical clearance is blocked):
    //       upDir = horizontal axis perpendicular to wall normal.
    //   • Floor / ceiling 1×2: upDir = player-facing axis snapped to NESW;
    //       normal = ±world-up.
    // Two extents are in play and intentionally different:
    //   • VISUAL ellipse extent (halfWidth=0.5, halfHeight=0.84375) — drawn
    //     by PortalRenderer. Matches Portal's 64×108 HU proportions exactly
    //     (PORTAL_HALF_WIDTH=32, PORTAL_HALF_HEIGHT=54 in portal_shareddefs.h).
    //   • FUNCTIONAL extent (halfWidth=0.5, halfHeight=1.0) — used by the
    //     teleport-crossing check (PortalRegistry.cpp:651), client teleport
    //     prediction (ClientPortalManager.cpp:297), wall passthrough fit
    //     (ClientPortalManager.cpp:143), and straddle ghost detection
    //     (ClientPortalManager.cpp:189). Has to cover the full 1×2 voxel
    //     wall opening so the passthrough/trigger areas stay in sync —
    //     voxels can't subdivide and shrinking the trigger to the visual
    //     ellipse would let the player walk into a non-portal slice of the
    //     wall hole and end up inside the destination wall.
    // Both extents are direction-agnostic — only the choice of upDir
    // distinguishes vertical / horizontal / floor / ceiling placements.
    struct Portal {
        glm::dvec3 origin{0.0};                    // center of the 1×2 rectangle, world-space
        glm::vec3  normal{0.0f, 0.0f, 1.0f};       // outward face normal (toward the player who fired)
        glm::vec3  upDir {0.0f, 1.0f, 0.0f};       // portal's local +Y (long axis, 2 blocks)
        glm::vec3  right {1.0f, 0.0f, 0.0f};       // = cross(upDir, normal) (short axis, 1 block)
        // The two wall block coordinates the portal is mounted on.
        // Tracked so that breaking either block can remove the portal.
        glm::ivec3 wallA{0};
        glm::ivec3 wallB{0};
        bool       active = false;
    };

    struct PortalPair {
        Portal blue;
        Portal orange;
    };

    class PortalRegistry {
    public:
        // Mint a new gun instance id. Stamped onto the gun's ItemStack via
        // the PORTAL_GUN_INSTANCE_ID DataComponent on first fire. Monotonic;
        // never re-uses ids even after a pair is removed (so stale references
        // simply look up empty rather than aliasing onto a fresh gun).
        uint64_t AllocId();

        // Validate + place. Replaces any prior portal of the same color on
        // the same gun. Walks an ordered list of CANDIDATE placements to
        // pick the first that fits — supports the user-facing fallback chain:
        //   wall click → vertical-up → vertical-down → horizontal-+R → -R
        //   floor click → forward → backward → +right → -right (player-aligned)
        //   ceiling click → same as floor (axes derived relative to player)
        // Returns Placed or Fizzled. The caller (PortalGunBehavior::OnGunUseOn)
        // maps Fizzled → UseResult::Fail.
        PlaceResult PlacePortal(uint64_t gunId, Game::World* world,
                                const BlockHitResult& hit, PortalColor color,
                                Server::ServerPlayer* player);

        // Wipe both portals belonging to a gun. Used by the shift+right-click
        // gesture (the user's "clear my portals" input). Drops any cached
        // per-player crossing state for the affected pair so a future re-fire
        // starts with a clean baseline.
        void ClearPair(uint64_t gunId);

        // Read pair (or nullptr if this gun has never fired). PortalGunBehavior
        // uses this to derive "what color goes next" — first shot blue,
        // second orange, every shot after that replaces orange (the user's
        // requested behaviour: blue stays put as the anchor, orange is the
        // moving target).
        const PortalPair* TryGetPair(uint64_t gunId) const;

        // Per-tick: detect player crossings + dispatch teleports. Walks every
        // active session via the server's session manager and updates the
        // per-player previous-side cache. Cheap — O(players × pairs).
        void Tick(Server::IntegratedServer* server);

        // Send PortalSetS2C for every currently-active portal to a single
        // connection. Called from IntegratedServer::OnPlayerJoined so a
        // freshly-arrived client renders the existing portals immediately
        // (rather than waiting for someone to re-fire them).
        void SyncToClient(Server::ServerConnection* connection) const;

        // Read-only iteration. Phase 4+ visuals key off it for rendering.
        const std::unordered_map<uint64_t, PortalPair>& All() const { return m_pairs; }

        // Called by the server's block-change pipeline whenever the world
        // changes at `pos`. Removes any portal whose wallA or wallB
        // matches — i.e. breaking (or replacing) either of the 2 wall
        // blocks behind a portal destroys it. Broadcasts the appropriate
        // PortalRemoveS2C and drops cached crossing state.
        void OnBlockChanged(const glm::ivec3& pos);

    private:
        uint64_t m_nextId = 1;
        std::unordered_map<uint64_t, PortalPair> m_pairs;

        // For each player, the player's center-of-body position (waist) on
        // the previous tick, per portal we're tracking against. Used to
        // detect plane crossings — we trigger teleport when the waist
        // center crosses from the +normal side to the -normal side of the
        // portal plane within the 1×2 oval bounds. Key = (gunId<<1)|color.
        //
        // Why this instead of the older "rising-edge inside-volume" trigger:
        // with the client-side wall passthrough (Physics.cpp's
        // PortalPassthroughFn) the player can now physically walk THROUGH
        // the wall in the portal opening. The old trigger fired the moment
        // the body first sampled the thin slab — at the wall surface.
        // Plane crossing fires when the body's geometric center crosses,
        // i.e. when half the body is already on the exit side. That gives
        // the "walk through" feel instead of "hug the wall + snap".
        //
        // Also tracks last-tick previous position globally per player
        // (m_prevPlayerPos) so PortalRegistry::Tick can derive the player's
        // velocity (= (curr − prev) / tickDt) without needing a new wire
        // field — used by the velocity-preservation path (#3).
        std::unordered_map<uint32_t,
            std::unordered_map<uint64_t, glm::dvec3>> m_prevWaist;
        std::unordered_map<uint32_t, glm::dvec3>     m_prevPlayerPos;

        // Per-player teleport cooldown (in ticks). Matches Valve's
        // PORTAL_COOLDOWN = 0.25 s → 5 ticks at 20 TPS. Guards against
        // network-race re-triggers (a delayed PlayerMoveC2S arriving with
        // pre-teleport position) and ping-pong between closely-placed
        // facing portals.
        std::unordered_map<uint32_t, int> m_teleportCooldown;
        static constexpr int kPostTeleportCooldownTicks = 5;
    };

    // Process-global singleton. Server-thread only.
    PortalRegistry& ServerRegistry();

} // namespace Game::Portal

#endif // ENABLE_PORTAL_GUN
