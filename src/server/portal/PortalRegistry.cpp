// File: src/server/portal/PortalRegistry.cpp
//
// See PortalRegistry.hpp for the high-level scope. This TU is the actual
// validation + crossing-detection logic.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalRegistry.hpp"

#include "common/core/Log.hpp"
#include "common/world/level/World.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "server/IntegratedServer.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/session/PlayerSession.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/PortalSetS2CPacket.hpp"
#include "common/network/packets/game/PortalRemoveS2CPacket.hpp"
#include "common/network/packets/game/PortalTeleportFlashS2CPacket.hpp"
#include "common/network/packets/game/PortalFizzleS2CPacket.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>

namespace Game::Portal {

    namespace {

        // Face-id → outward normal. Matches the convention used elsewhere in
        // the codebase (UseOnContext::getPlacementPos): the offset directions
        // are the side of the block that is OUTSIDE / facing the player who
        // clicked, so the normal is that same direction.
        glm::ivec3 FaceNormal(int face) {
            switch (face) {
                case 0: return { 0, -1,  0}; // bottom
                case 1: return { 0,  1,  0}; // top
                case 2: return { 0,  0, -1}; // north
                case 3: return { 0,  0,  1}; // south
                case 4: return {-1,  0,  0}; // west
                case 5: return { 1,  0,  0}; // east
            }
            return {0, 0, 0};
        }

        // Vertical wall = the face is one of the four horizontal directions.
        bool IsVerticalWall(int face) { return face >= 2 && face <= 5; }
        bool IsFloor(int face)        { return face == 1; }   // hit top of block; portal lies on the floor
        bool IsCeiling(int face)      { return face == 0; }   // hit bottom of block; portal hangs from ceiling

        // Snap a yaw to the nearest cardinal as an integer unit vector.
        //
        // Project convention: yaw values stored on ServerPlayer come straight
        // from the client's `camera.yaw` (sent via PlayerMoveC2S). Camera
        // convention uses forward = (cos(yaw), 0, sin(yaw)), i.e.:
        //   yaw   0  → +X = east
        //   yaw  90  → +Z = south
        //   yaw 180  → -X = west
        //   yaw -90  → -Z = north
        // (NOT the MC convention you'd guess from the server-thread name.)
        glm::ivec3 PlayerFacingHorizontal(float yawDeg) {
            float y = std::fmod(std::fmod(yawDeg, 360.0f) + 360.0f, 360.0f);
            int q = static_cast<int>(std::round(y / 90.0f)) & 3;
            switch (q) {
                case 0: return { 1, 0,  0}; // east
                case 1: return { 0, 0,  1}; // south
                case 2: return {-1, 0,  0}; // west
                case 3: return { 0, 0, -1}; // north
            }
            return {1, 0, 0};
        }

        // Pack (gunId, color) into a single 64-bit key for the per-player
        // signed-distance cache.
        uint64_t PortalKey(uint64_t gunId, PortalColor c) {
            return (gunId << 1) | static_cast<uint64_t>(c);
        }

        // True iff the block is a full opaque cube — the only valid surface
        // a portal can stick to. Mirrors the user-visible Portal-game rule
        // (no portals on glass, fences, slabs, water, etc.). We piggyback
        // on the block's `opaque` flag because every full-cube opaque block
        // in our registry is also a flat-faced cube — there's no "opaque
        // but model-altered" entry in the table today. If/when slabs land
        // and want to also be opaque, this rule will need a dedicated
        // `isFullCube` field on Block (mirrors MC's CollisionContext path).
        bool IsValidPortalSurface(Game::World* world, const glm::ivec3& pos) {
            const BlockID id = world->GetBlock(pos.x, pos.y, pos.z);
            if (id == BlockID::Air) return false;
            return BlockRegistry::Get(id).opaque;
        }

        // True iff the block is air (or any non-solid pass-through). The
        // portal occupies these blocks so the player can step into the
        // portal volume without colliding with geometry.
        bool IsAirSpace(Game::World* world, const glm::ivec3& pos) {
            return world->GetBlock(pos.x, pos.y, pos.z) == BlockID::Air;
        }

        // Player AABB half-extents — matches the player physics elsewhere in
        // the codebase (0.6 wide, 1.8 tall). Used to test whether the player's
        // body sweep through the portal plane lies within the 1×2 oval.
        constexpr double kPlayerHalfWidth  = 0.30;
        constexpr double kPlayerHeight     = 1.80;
        constexpr double kPlayerEyeHeight  = 1.62;

        // Build the 4×4 transform whose columns are [right, up, normal,
        // origin] (with the basis cross-checked to be orthonormal). This is
        // the portal's local→world matrix. Inverse of an orthonormal basis
        // is its transpose, so we never need a real matrix inverse.
        glm::dmat4 PortalToWorld(const Portal& p) {
            glm::dmat4 m(1.0);
            m[0] = glm::dvec4(p.right,  0.0);
            m[1] = glm::dvec4(p.upDir,  0.0);
            m[2] = glm::dvec4(p.normal, 0.0);
            m[3] = glm::dvec4(p.origin, 1.0);
            return m;
        }

        glm::dmat4 WorldToPortal(const Portal& p) {
            // Orthonormal-basis inverse: transpose of rotation, negated translation.
            glm::dmat3 R(glm::dvec3(p.right),
                         glm::dvec3(p.upDir),
                         glm::dvec3(p.normal));
            glm::dmat3 Rt = glm::transpose(R);
            glm::dvec3 t  = -(Rt * p.origin);
            glm::dmat4 m(1.0);
            m[0] = glm::dvec4(Rt[0], 0.0);
            m[1] = glm::dvec4(Rt[1], 0.0);
            m[2] = glm::dvec4(Rt[2], 0.0);
            m[3] = glm::dvec4(t,     1.0);
            return m;
        }

        // Portal-classic destination matrix:
        //   M = T_dst · Mirror180Up · inverse(T_src)
        // Mirror180Up flips x (right) and z (normal) but keeps y (up). This
        // is what makes a player who walked INTO the source portal exit the
        // destination portal facing OUT (rather than backwards into the wall).
        glm::dmat4 SrcToDst(const Portal& src, const Portal& dst) {
            glm::dmat4 mirror(1.0);
            mirror[0][0] = -1.0;  // flip right
            mirror[2][2] = -1.0;  // flip normal
            return PortalToWorld(dst) * mirror * WorldToPortal(src);
        }

    } // namespace

    // (Old volume-presence helpers PlayerInsidePortalVolume /
    // SampleInsidePortalVolume / PlayerOnFrontSide were removed when the
    // teleport trigger switched to plane-crossing detection — see Tick()
    // below for the new logic.)

    PortalRegistry& ServerRegistry() {
        static PortalRegistry instance;
        return instance;
    }

    uint64_t PortalRegistry::AllocId() {
        return m_nextId++;
    }

    namespace {

        // Tiny outward offset so portal-plane sampling and Phase 4+ render
        // depth don't z-fight with the wall surface. ~1 mm is invisible
        // and well above any reasonable depth-buffer epsilon.
        constexpr double kSurfaceOffset = 0.001;

        // A trial placement. Built by EnumerateCandidates and consumed by
        // ValidateCandidate — represents "what would the portal look like
        // if I extended in this direction, and which world cells does that
        // require to be wall vs. air?"
        struct PortalCandidate {
            glm::ivec3 wallA, wallB;   // both must be opaque full cubes
            glm::ivec3 airA,  airB;    // both must be empty (so the portal volume isn't inside geometry)
            glm::dvec3 origin;
            glm::vec3  normal, upDir, right;
            const char* label;         // for fizzle log diagnostics ("vertical-up", "horizontal-east", …)
        };

        // Build a candidate from primitives. `hitBlock` = the block the
        // raycast hit (one of the two wall blocks). `normalI` = outward
        // normal (toward the player). `extendI` = direction in which the
        // portal's LONG axis extends (= the second wall block's offset).
        // Both `normalI` and `extendI` are integer ±unit vectors with
        // disjoint nonzero axes, so cross(extendI, normalI) is also a unit
        // integer vector — used as the portal's `right` (short-axis basis).
        PortalCandidate MakeCandidate(glm::ivec3 hitBlock,
                                      glm::ivec3 normalI,
                                      glm::ivec3 extendI,
                                      const char* label) {
            PortalCandidate c;
            c.wallA = hitBlock;
            c.wallB = hitBlock + extendI;
            c.airA  = hitBlock + normalI;
            c.airB  = c.wallB + normalI;
            c.normal = glm::vec3(normalI);
            c.upDir  = glm::vec3(extendI);
            c.right  = glm::cross(c.upDir, c.normal); // unit, perpendicular, right-handed
            // Origin = midpoint of the two wall block centers + half-block
            // outward (to land exactly on the wall face), + epsilon offset.
            const glm::dvec3 wACenter = glm::dvec3(c.wallA) + glm::dvec3(0.5);
            const glm::dvec3 wBCenter = glm::dvec3(c.wallB) + glm::dvec3(0.5);
            const glm::dvec3 mid      = (wACenter + wBCenter) * 0.5;
            c.origin = mid + glm::dvec3(c.normal) * (0.5 + kSurfaceOffset);
            c.label  = label;
            return c;
        }

        bool ValidateCandidate(Game::World* world, const PortalCandidate& c) {
            return IsValidPortalSurface(world, c.wallA)
                && IsValidPortalSurface(world, c.wallB)
                && IsAirSpace(world, c.airA)
                && IsAirSpace(world, c.airB);
        }

        // ── Network broadcast helpers ───────────────────────────────────────
        //
        // Both helpers walk every active session via the global integrated
        // server's session manager and send the packet to each connection
        // (INCLUDING the firing player — the firing client also needs to see
        // its own portals to render them in Phase 4). On a server with no
        // sessions yet (early startup, or feature-on/no-clients), these are
        // safe no-ops.
        Network::PortalSetS2CPacket BuildSetPacket(uint64_t gunId,
                                                   PortalColor color,
                                                   const Portal& p) {
            Network::PortalSetS2CPacket pk;
            pk.gunId   = gunId;
            pk.color   = static_cast<uint8_t>(color);
            pk.originX = p.origin.x;
            pk.originY = p.origin.y;
            pk.originZ = p.origin.z;
            pk.normalX = p.normal.x;
            pk.normalY = p.normal.y;
            pk.normalZ = p.normal.z;
            pk.upX     = p.upDir.x;
            pk.upY     = p.upDir.y;
            pk.upZ     = p.upDir.z;
            return pk;
        }

        void BroadcastPortalSet(uint64_t gunId, PortalColor color, const Portal& p) {
            if (!Server::g_integratedServer) return;
            auto* mgr = Server::g_integratedServer->GetSessionManager();
            if (!mgr) return;
            const auto packet = BuildSetPacket(gunId, color, p);
            const auto data   = Network::Serialization::Serialize(packet);
            for (auto& session : mgr->GetAllSessions()) {
                if (!session) continue;
                auto* conn = session->GetConnection();
                if (!conn) continue;
                conn->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PortalSetS2C), data);
            }
        }

        void BroadcastPortalRemove(uint64_t gunId, uint8_t colorByte) {
            if (!Server::g_integratedServer) return;
            auto* mgr = Server::g_integratedServer->GetSessionManager();
            if (!mgr) return;
            Network::PortalRemoveS2CPacket packet;
            packet.gunId = gunId;
            packet.color = colorByte;
            const auto data = Network::Serialization::Serialize(packet);
            for (auto& session : mgr->GetAllSessions()) {
                if (!session) continue;
                auto* conn = session->GetConnection();
                if (!conn) continue;
                conn->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PortalRemoveS2C), data);
            }
        }

        // Reasons mirror PortalParticleSystem::BurstKind on the client.
        // Keep these byte values in lockstep (BadSurface=0, Close=1).
        constexpr uint8_t kFizzleBadSurface = 0;
        constexpr uint8_t kFizzleClose      = 1;

        void BroadcastPortalFizzle(const glm::dvec3& origin,
                                   const glm::vec3& normal,
                                   uint8_t colorByte, uint8_t reason) {
            if (!Server::g_integratedServer) return;
            auto* mgr = Server::g_integratedServer->GetSessionManager();
            if (!mgr) return;
            Network::PortalFizzleS2CPacket packet;
            packet.originX = origin.x;
            packet.originY = origin.y;
            packet.originZ = origin.z;
            packet.normalX = normal.x;
            packet.normalY = normal.y;
            packet.normalZ = normal.z;
            packet.color   = colorByte;
            packet.reason  = reason;
            const auto data = Network::Serialization::Serialize(packet);
            for (auto& session : mgr->GetAllSessions()) {
                if (!session) continue;
                auto* conn = session->GetConnection();
                if (!conn) continue;
                conn->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PortalFizzleS2C), data);
            }
        }

        void BroadcastTeleportFlash(uint64_t gunId) {
            if (!Server::g_integratedServer) return;
            auto* mgr = Server::g_integratedServer->GetSessionManager();
            if (!mgr) return;
            Network::PortalTeleportFlashS2CPacket packet;
            packet.gunId = gunId;
            const auto data = Network::Serialization::Serialize(packet);
            for (auto& session : mgr->GetAllSessions()) {
                if (!session) continue;
                auto* conn = session->GetConnection();
                if (!conn) continue;
                conn->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PortalTeleportFlashS2C), data);
            }
        }

    } // namespace

    const PortalPair* PortalRegistry::TryGetPair(uint64_t gunId) const {
        auto it = m_pairs.find(gunId);
        return (it == m_pairs.end()) ? nullptr : &it->second;
    }

    void PortalRegistry::ClearPair(uint64_t gunId) {
        auto it = m_pairs.find(gunId);
        if (it == m_pairs.end()) return;
        // Broadcast a close burst for each active portal BEFORE erasing
        // the pair, since the burst origin needs the old portal pose.
        if (it->second.blue.active) {
            BroadcastPortalFizzle(it->second.blue.origin,
                                  it->second.blue.normal,
                                  /*color=*/0, kFizzleClose);
        }
        if (it->second.orange.active) {
            BroadcastPortalFizzle(it->second.orange.origin,
                                  it->second.orange.normal,
                                  /*color=*/1, kFizzleClose);
        }
        m_pairs.erase(it);
        // Drop both color cache entries on every player. Cheap — bounded
        // by player count, and the caches are tiny.
        const uint64_t kBlue   = PortalKey(gunId, PortalColor::Blue);
        const uint64_t kOrange = PortalKey(gunId, PortalColor::Orange);
        for (auto& [pid, m] : m_prevWaist) {
            m.erase(kBlue);
            m.erase(kOrange);
        }
        // color = 2 → BOTH (whole pair gone). Phase 2 only ever clears whole
        // pairs (the user's shift-clear gesture). Per-color removes are
        // reserved on the wire format for a later "single-portal removal"
        // event — none exists yet.
        BroadcastPortalRemove(gunId, /*color=*/2);
        Log::Info("[PortalGun] Cleared portal pair for gun=%llu",
                  static_cast<unsigned long long>(gunId));
    }

    void PortalRegistry::OnBlockChanged(const glm::ivec3& pos) {
        // Walk every pair and drop any portal whose wallA or wallB
        // matches the changed block. Per-color drop so an orphaned
        // sibling on a different wall keeps rendering as an inactive
        // portal.
        for (auto it = m_pairs.begin(); it != m_pairs.end(); ) {
            const uint64_t gunId = it->first;
            PortalPair& pair = it->second;

            auto destroy = [&](Portal& portal, PortalColor color) {
                if (!portal.active) return;
                if (portal.wallA != pos && portal.wallB != pos) return;
                // Snapshot pose before clearing so the close burst lands
                // at the doomed portal's actual location.
                const glm::dvec3 burstOrigin = portal.origin;
                const glm::vec3  burstNormal = portal.normal;
                portal.active = false;
                portal.wallA = glm::ivec3(0);
                portal.wallB = glm::ivec3(0);
                const uint64_t key = PortalKey(gunId, color);
                for (auto& [pid, m] : m_prevWaist) m.erase(key);
                BroadcastPortalRemove(gunId,
                    static_cast<uint8_t>(color));
                BroadcastPortalFizzle(burstOrigin, burstNormal,
                    static_cast<uint8_t>(color), kFizzleClose);
                Log::Info("[PortalGun] Block at (%d,%d,%d) broken — "
                          "removed %s portal for gun=%llu",
                          pos.x, pos.y, pos.z,
                          color == PortalColor::Blue ? "BLUE" : "ORANGE",
                          static_cast<unsigned long long>(gunId));
            };

            destroy(pair.blue,   PortalColor::Blue);
            destroy(pair.orange, PortalColor::Orange);

            // Drop the pair entry once both colors are gone so the
            // gun's PORTAL_GUN_INSTANCE_ID can re-fire from scratch.
            if (!pair.blue.active && !pair.orange.active) {
                it = m_pairs.erase(it);
            } else {
                ++it;
            }
        }
    }

    void PortalRegistry::SyncToClient(Server::ServerConnection* connection) const {
        if (!connection) return;
        for (const auto& [gunId, pair] : m_pairs) {
            if (pair.blue.active) {
                const auto packet = BuildSetPacket(gunId, PortalColor::Blue,   pair.blue);
                const auto data   = Network::Serialization::Serialize(packet);
                connection->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PortalSetS2C), data);
            }
            if (pair.orange.active) {
                const auto packet = BuildSetPacket(gunId, PortalColor::Orange, pair.orange);
                const auto data   = Network::Serialization::Serialize(packet);
                connection->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PortalSetS2C), data);
            }
        }
    }

    PlaceResult PortalRegistry::PlacePortal(uint64_t gunId, Game::World* world,
                                            const BlockHitResult& hit,
                                            PortalColor color,
                                            Server::ServerPlayer* player) {
        if (!world) return PlaceResult::Fizzled;

        const glm::ivec3 normalI = FaceNormal(hit.face);
        if (normalI == glm::ivec3(0)) {
            Log::Info("[PortalGun] Fizzle: invalid face %d", hit.face);
            // No usable normal — burst with world-up so it still renders.
            BroadcastPortalFizzle(glm::dvec3(hit.hitPoint),
                                  glm::vec3(0.0f, 1.0f, 0.0f),
                                  static_cast<uint8_t>(color),
                                  kFizzleBadSurface);
            return PlaceResult::Fizzled;
        }

        // Build the priority-ordered candidate list per face type. The
        // user-visible rule is: the FIRST candidate that fits wins, so the
        // ordering encodes user intent (vertical first on walls, player-
        // aligned first on floors / ceilings, perpendicular fallback last).
        std::vector<PortalCandidate> candidates;
        candidates.reserve(4);

        if (IsVerticalWall(hit.face)) {
            // Vertical wall basis: the two "extend" axes are world up/down
            // (for 1×2) and the horizontal-perpendicular-to-wall axis (for
            // 2×1, the user's "horizontal portal on the wall" fallback).
            const glm::ivec3 worldUp  {0, 1, 0};
            const glm::ivec3 worldDown{0, -1, 0};
            // horizPerp = horizontal axis in the wall's plane. Two choices,
            // ±, since we don't know which way the user expects the 2×1
            // portal to extend — try both, +right first.
            const glm::ivec3 horizPerp = glm::ivec3(glm::cross(
                glm::vec3(worldUp), glm::vec3(normalI)));

            candidates.push_back(MakeCandidate(hit.blockPos, normalI, worldUp,    "vertical-up"));
            candidates.push_back(MakeCandidate(hit.blockPos, normalI, worldDown,  "vertical-down"));
            candidates.push_back(MakeCandidate(hit.blockPos, normalI,  horizPerp, "horizontal-+R"));
            candidates.push_back(MakeCandidate(hit.blockPos, normalI, -horizPerp, "horizontal--R"));
        } else if (IsFloor(hit.face) || IsCeiling(hit.face)) {
            // Floor / ceiling: the portal lies flat. The "long" axis (upDir
            // in portal-local space) is the player's primary facing snapped
            // to NESW. Falls back to perpendicular if the player-aligned
            // 1×2 strip doesn't have room.
            const float yaw = player ? player->getYaw() : 0.0f;
            const glm::ivec3 fwd = PlayerFacingHorizontal(yaw);
            // sideways = cross(facing, normal) — unit horizontal perpendicular.
            const glm::ivec3 side = glm::ivec3(glm::cross(
                glm::vec3(fwd), glm::vec3(normalI)));

            candidates.push_back(MakeCandidate(hit.blockPos, normalI,  fwd,  "facing-forward"));
            candidates.push_back(MakeCandidate(hit.blockPos, normalI, -fwd,  "facing-backward"));
            candidates.push_back(MakeCandidate(hit.blockPos, normalI,  side, "facing-+side"));
            candidates.push_back(MakeCandidate(hit.blockPos, normalI, -side, "facing--side"));
        } else {
            Log::Info("[PortalGun] Fizzle: invalid face %d", hit.face);
            BroadcastPortalFizzle(glm::dvec3(hit.hitPoint),
                                  glm::vec3(normalI),
                                  static_cast<uint8_t>(color),
                                  kFizzleBadSurface);
            return PlaceResult::Fizzled;
        }

        // First candidate that validates wins. Logging the rejected ones
        // makes "why didn't my portal stick?" diagnosable in the server log.
        const PortalCandidate* chosen = nullptr;
        for (const auto& c : candidates) {
            if (ValidateCandidate(world, c)) { chosen = &c; break; }
        }
        if (!chosen) {
            Log::Info("[PortalGun] Fizzle: no candidate fits at "
                      "(%d,%d,%d) face=%d (tried %zu orientations)",
                      hit.blockPos.x, hit.blockPos.y, hit.blockPos.z,
                      hit.face, candidates.size());
            BroadcastPortalFizzle(glm::dvec3(hit.hitPoint),
                                  glm::vec3(normalI),
                                  static_cast<uint8_t>(color),
                                  kFizzleBadSurface);
            return PlaceResult::Fizzled;
        }

        Portal p;
        p.origin = chosen->origin;
        p.normal = chosen->normal;
        p.upDir  = chosen->upDir;
        p.right  = chosen->right;
        p.wallA  = chosen->wallA;
        p.wallB  = chosen->wallB;
        p.active = true;

        // Replace the same color on this gun. Auto-creates the pair entry.
        PortalPair& pair = m_pairs[gunId];
        // If a portal of the same color was already active, broadcast a
        // close burst for the OLD position before overwriting — gives the
        // player visual feedback that "the previous portal collapsed
        // because you re-fired this color."
        {
            const Portal& prev = (color == PortalColor::Blue) ? pair.blue : pair.orange;
            if (prev.active) {
                BroadcastPortalFizzle(prev.origin, prev.normal,
                    static_cast<uint8_t>(color), kFizzleClose);
            }
        }
        if (color == PortalColor::Blue) pair.blue   = p;
        else                            pair.orange = p;

        // Drop cached crossing state for this portal so the FIRST tick after
        // placement doesn't trigger a spurious teleport for any player who
        // happens to be inside the portal volume the moment it spawns.
        const uint64_t key = PortalKey(gunId, color);
        for (auto& [pid, m] : m_prevWaist) m.erase(key);

        Log::Info("[PortalGun] Placed %s portal for gun=%llu at "
                  "origin=(%.2f,%.2f,%.2f) normal=(%.0f,%.0f,%.0f) "
                  "orientation=%s",
                  color == PortalColor::Blue ? "BLUE" : "ORANGE",
                  static_cast<unsigned long long>(gunId),
                  p.origin.x, p.origin.y, p.origin.z,
                  p.normal.x, p.normal.y, p.normal.z,
                  chosen->label);

        // Broadcast the placement (or move) to every connected client. The
        // firing client also receives this — it's how the local renderer
        // (Phase 4) finds out where to draw the portal.
        BroadcastPortalSet(gunId, color, p);
        return PlaceResult::Placed;
    }

    void PortalRegistry::Tick(Server::IntegratedServer* server) {
        if (!server || m_pairs.empty()) return;

        auto* sessionManager = server->GetSessionManager();
        if (!sessionManager) return;

        auto sessions = sessionManager->GetAllSessions();
        if (sessions.empty()) return;

        // Decrement cooldowns once per tick.
        for (auto& [pid, c] : m_teleportCooldown) {
            if (c > 0) --c;
        }

        // 20 TPS — used to convert per-tick position delta into blocks/sec
        // for the velocity packet field.
        constexpr double kTickDt = 1.0 / 20.0;

        for (auto& session : sessions) {
            if (!session) continue;
            Server::ServerPlayer* player = session->GetPlayer();
            Server::ServerConnection* conn = session->GetConnection();
            if (!player) continue;

            const uint32_t pid = player->getPlayerId();
            const glm::dvec3 pos = player->getPosition();
            // Eye = the trigger reference. Mirrors the client's eye-
            // based trigger in CheckEyeCrossing — fires the moment the
            // eye reaches the source plane (Portal-feel: "you went
            // fully through" rather than "you were half through").
            // Standing eye height = 1.62 m above feet.
            constexpr double kEyeHeight = 1.62;
            const glm::dvec3 eye =
                pos + glm::dvec3(0.0, kEyeHeight, 0.0);
            auto& waistMap = m_prevWaist[pid];   // map name kept; now stores eye

            // Player velocity for #3 (velocity preservation through the
            // portal). Derived from waist-position delta this tick — no
            // wire change to PlayerMoveC2S needed. First observation
            // produces zero velocity (no prev sample to diff against).
            glm::dvec3 velocity(0.0);
            auto prevPlayerIt = m_prevPlayerPos.find(pid);
            if (prevPlayerIt != m_prevPlayerPos.end()) {
                velocity = (pos - prevPlayerIt->second) / kTickDt;
            }
            m_prevPlayerPos[pid] = pos;

            for (auto& [gunId, pair] : m_pairs) {
                struct Side { PortalColor color; const Portal* self; const Portal* other; };
                const Side sides[2] = {
                    { PortalColor::Blue,   &pair.blue,   &pair.orange },
                    { PortalColor::Orange, &pair.orange, &pair.blue   },
                };

                for (const Side& s : sides) {
                    if (!s.self->active) continue;

                    const uint64_t key = PortalKey(gunId, s.color);
                    auto prevIt = waistMap.find(key);
                    const bool isFirstObs = (prevIt == waistMap.end());
                    const glm::dvec3 prevEye =
                        isFirstObs ? eye : prevIt->second;
                    waistMap[key] = eye;
                    if (isFirstObs) continue;

                    if (m_teleportCooldown[pid] > 0) continue;

                    // Eye in close zone. Drop edge-detection so a
                    // stopped player inside the zone still teleports
                    // (matches client; see ClientPortalManager.cpp).
                    constexpr double kEarlyPredictDistance = 0.085;
                    const glm::dvec3 nrm = glm::dvec3(s.self->normal);
                    const double currSigned = glm::dot(eye - s.self->origin, nrm);
                    // Eye crossing check — must have approached from
                    // the +normal side last tick (matches client; see
                    // ClientPortalManager.cpp). Handles fast falls
                    // that overshoot the window AND rejects "always
                    // behind the wall" players.
                    if (currSigned > kEarlyPredictDistance) continue;
                    const double prevSigned =
                        glm::dot(prevEye - s.self->origin, nrm);
                    if (prevSigned <= 0.0) continue;
                    if (glm::dot(velocity, nrm) > 0.5) continue;  // walking out = no trigger

                    if (glm::length(eye - prevEye) > 3.0) {
                        m_teleportCooldown[pid] = kPostTeleportCooldownTicks;
                        continue;
                    }

                    // Lateral check using current eye.
                    const glm::dvec3 d = eye - s.self->origin;
                    const double su = glm::dot(d, glm::dvec3(s.self->right));
                    const double tu = glm::dot(d, glm::dvec3(s.self->upDir));
                    if (std::abs(su) > 0.5 || std::abs(tu) > 1.0) continue;

                    // Sibling must be active to have a destination.
                    if (!s.other->active) {
                        Log::Info("[PortalGun] gun=%llu: %s crossed but sibling not placed",
                                  static_cast<unsigned long long>(gunId),
                                  s.color == PortalColor::Blue ? "BLUE" : "ORANGE");
                        continue;
                    }

                    // Position transform: take eye through M, derive
                    // feet by subtracting eye height (matches client).
                    const glm::dmat4 M = SrcToDst(*s.self, *s.other);
                    const glm::dvec3 newEye = glm::dvec3(M * glm::dvec4(eye, 1.0));
                    glm::dvec3 newPos = newEye - glm::dvec3(0.0, kEyeHeight, 0.0);
                    glm::dvec3 newVel = glm::dmat3(M) * velocity;

                    // Y/wall override — eye just past dst plane on
                    // +dst.normal side (matches client). For wall dst,
                    // push the body out of the wall along +dst.normal
                    // so the camera doesn't end up inside the wall
                    // block; mirrors ClientPortalManager.cpp.
                    constexpr double kEyeOffsetFromPlane = 0.086;
                    const float dstNy = s.other->normal.y;
                    if (dstNy > 0.7f) {
                        newPos.y = s.other->origin.y + kEyeOffsetFromPlane - kEyeHeight;
                    } else if (dstNy < -0.7f) {
                        // Ceiling: use a larger offset (0.2 m) so the
                        // head doesn't poke into the ceiling block
                        // AND the eye is well outside the trigger
                        // window (matches client; see
                        // ClientPortalManager.cpp).
                        constexpr double kCeilingEyeOffset = 0.1;
                        newPos.y = s.other->origin.y - kCeilingEyeOffset - kEyeHeight;
                    } else {
                        // Vertical wall — push eye slightly OFF the
                        // dst plane in +dst.normal direction (matches
                        // client; see ClientPortalManager.cpp).
                        const glm::dvec3 currEyePos =
                            newPos + glm::dvec3(0.0, kEyeHeight, 0.0);
                        const double currEyeSd = glm::dot(
                            currEyePos - s.other->origin, glm::dvec3(s.other->normal));
                        const double adjustment = kEyeOffsetFromPlane - currEyeSd;
                        newPos += glm::dvec3(s.other->normal) * adjustment;

                        // Floor/ceiling-source → wall-dest puts the
                        // EYE at the wall portal's Y center, leaving
                        // the FEET 1.62 m below (typically inside the
                        // floor block beneath the portal). Clamp feet
                        // to the wall portal's bottom edge.
                        const double portalBottomY =
                            s.other->origin.y - 1.0;
                        if (newPos.y < portalBottomY) {
                            newPos.y = portalBottomY;
                        }
                    }

                    // Minimum upward exit velocity for floor portals.
                    if (dstNy > 0.7f) {
                        constexpr double kMinFloorExitVelocity = 12.0;
                        if (newVel.y < kMinFloorExitVelocity) {
                            newVel.y = kMinFloorExitVelocity;
                        }
                    }

                    // Rotate the player's full forward vector through M
                    // (using yaw + pitch) so the see-through view's
                    // pitch agrees with the post-teleport pitch. Without
                    // this, going wall→floor leaves pitch at the entry
                    // value (e.g. 0°) while the see-through view shows
                    // ~90° — visible as a 90° tilt the moment the
                    // teleport fires.
                    const float yawRad   = glm::radians(player->getYaw());
                    const float pitchRad = glm::radians(player->getPitch());
                    const double cp = std::cos(pitchRad);
                    const glm::dvec3 fwd(
                        std::cos(yawRad) * cp,
                        std::sin(pitchRad),
                        std::sin(yawRad) * cp);
                    const glm::dvec3 newFwd =
                        glm::normalize(glm::dmat3(M) * fwd);

                    float newPitchDeg = glm::degrees(std::asin(
                        std::clamp<double>(newFwd.y, -1.0, 1.0)));
                    newPitchDeg = std::clamp(newPitchDeg, -89.5f, 89.5f);

                    float newYawDeg;
                    const double horizLen2 =
                        newFwd.x * newFwd.x + newFwd.z * newFwd.z;
                    if (horizLen2 > 0.01) {
                        newYawDeg = glm::degrees(
                            std::atan2(newFwd.z, newFwd.x));
                    } else if (std::abs(s.other->normal.y) > 0.7f) {
                        // Floor/ceiling exit — align with -upDir.
                        newYawDeg = glm::degrees(std::atan2(
                            -s.other->upDir.z, -s.other->upDir.x));
                    } else {
                        // Wall dst with degenerate horizontal forward
                        // — recover yaw from M-rotated RIGHT vector
                        // (matches client; see ClientPortalManager.cpp).
                        const glm::dvec3 rightH(
                            -std::sin(yawRad), 0.0, std::cos(yawRad));
                        const glm::dvec3 newRight = glm::normalize(
                            glm::dmat3(M) * rightH);
                        newYawDeg = glm::degrees(
                            std::atan2(-newRight.x, newRight.z));
                    }

                    // Any horizontal↔horizontal pair preserves the
                    // player's view (matches client; see
                    // ClientPortalManager.cpp).
                    const bool srcHoriz = std::abs(s.self->normal.y) > 0.7f;
                    const bool dstHoriz = std::abs(s.other->normal.y) > 0.7f;
                    if (srcHoriz && dstHoriz) {
                        newYawDeg   = player->getYaw();
                        newPitchDeg = player->getPitch();
                    }


                    Log::Info("[PortalGun] gun=%llu: TELEPORT player %u %s→%s "
                              "pos(%.2f,%.2f,%.2f)→(%.2f,%.2f,%.2f) "
                              "vel(%.2f,%.2f,%.2f)→(%.2f,%.2f,%.2f) "
                              "yaw %.1f→%.1f",
                              static_cast<unsigned long long>(gunId), pid,
                              s.color == PortalColor::Blue ? "BLUE" : "ORANGE",
                              s.color == PortalColor::Blue ? "ORANGE" : "BLUE",
                              pos.x, pos.y, pos.z,
                              newPos.x, newPos.y, newPos.z,
                              velocity.x, velocity.y, velocity.z,
                              newVel.x, newVel.y, newVel.z,
                              player->getYaw(), newYawDeg);

                    if (conn) {
                        conn->Teleport(newPos.x, newPos.y, newPos.z,
                                       newYawDeg, newPitchDeg,
                                       newVel.x, newVel.y, newVel.z);
                        player->setRotation(newYawDeg, newPitchDeg);
                    } else {
                        player->teleport(newPos);
                        player->setRotation(newYawDeg, newPitchDeg);
                    }

                    // Invalidate THIS player's caches — post-teleport, prev
                    // values would point to the source side and trigger a
                    // bogus back-crossing next tick.
                    waistMap.clear();
                    m_prevPlayerPos[pid] = newPos;
                    m_teleportCooldown[pid] = kPostTeleportCooldownTicks;

                    BroadcastTeleportFlash(gunId);
                    goto nextPlayer;
                }
            }
            nextPlayer:;
        }

        // Garbage-collect cache entries for disconnected players.
        if (m_prevWaist.size() > sessions.size() * 2 ||
            m_prevPlayerPos.size() > sessions.size() * 2) {
            std::unordered_map<uint32_t, bool> live;
            for (auto& s : sessions) {
                if (s && s->GetPlayer()) live[s->GetPlayer()->getPlayerId()] = true;
            }
            for (auto it = m_prevWaist.begin(); it != m_prevWaist.end();) {
                if (!live.count(it->first)) it = m_prevWaist.erase(it);
                else                        ++it;
            }
            for (auto it = m_prevPlayerPos.begin(); it != m_prevPlayerPos.end();) {
                if (!live.count(it->first)) it = m_prevPlayerPos.erase(it);
                else                        ++it;
            }
            for (auto it = m_teleportCooldown.begin(); it != m_teleportCooldown.end();) {
                if (!live.count(it->first)) it = m_teleportCooldown.erase(it);
                else                        ++it;
            }
        }
    }

} // namespace Game::Portal

#endif // ENABLE_PORTAL_GUN
