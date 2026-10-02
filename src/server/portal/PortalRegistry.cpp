// File: src/server/portal/PortalRegistry.cpp
//
// See PortalRegistry.hpp for the high-level scope. This TU is the actual
// validation + crossing-detection logic.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalRegistry.hpp"
#include "common/core/Mth.hpp"

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
#include "common/world/portal/PortalState.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "server/portal/ImmersivePortalRegistry.hpp"
#include <glm/gtc/quaternion.hpp>
#endif
#include "server/level/ServerLevel.hpp"
#include "server/world/storage/anvil/PlayerUuid.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
        // Yaw is MC's throughout the engine now (0 = +Z south, clockwise), the
        // same value the client's camera stores and PlayerMoveC2S carries.
        glm::ivec3 PlayerFacingHorizontal(float yawDeg) {
            float y = std::fmod(std::fmod(yawDeg, 360.0f) + 360.0f, 360.0f);
            int q = static_cast<int>(std::round(y / 90.0f)) & 3;
            switch (q) {
                case 0: return { 0, 0,  1}; // south
                case 1: return {-1, 0,  0}; // west
                case 2: return { 0, 0, -1}; // north
                case 3: return { 1, 0,  0}; // east
            }
            return {0, 0, 1};
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

        // Build a candidate from primitives. `anchorBlock` = the wall block
        // at the portal's BOTTOM (local -up) end. `normalI` = outward normal
        // (toward the player). `upI` = the portal's up: the LONG axis, so the
        // second wall block is anchor + up. Both are integer ±unit vectors
        // with disjoint nonzero axes, so cross(upI, normalI) is also a unit
        // integer vector — used as the portal's `right` (short-axis basis).
        //
        // The orientation is FIXED by the caller; a candidate only ever
        // differs by where the anchor sits (Portal 1 "bumps" a portal into
        // a spot that fits, it never rotates it — a portal is never upside
        // down or sideways). The old candidate set extended the strip in
        // every direction and made that direction the portal's up, which is
        // how a wall shot near a ceiling came out upside down and a floor
        // shot near a wall came out facing the wrong way.
        PortalCandidate MakeCandidate(glm::ivec3 anchorBlock,
                                      glm::ivec3 normalI,
                                      glm::ivec3 upI,
                                      const char* label) {
            PortalCandidate c;
            c.wallA = anchorBlock;
            c.wallB = anchorBlock + upI;
            c.airA  = anchorBlock + normalI;
            c.airB  = c.wallB + normalI;
            c.normal = glm::vec3(normalI);
            c.upDir  = glm::vec3(upI);
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

        // ── Footprint (grid and free placement alike) ───────────────────
        // The wall cells behind a portal's 1×2 opening: every cell of the
        // wall layer its rectangle (±0.5 along right, ±1.0 along up)
        // overlaps. A grid-aligned portal covers exactly wallA and wallB; a
        // free one (/gamerule portal_gun_free_placement) up to 2×3. The air
        // cells in front are these + normal. Returns the count (≤ 6).
        int AxisOf(const glm::ivec3& v) { return v.x != 0 ? 0 : (v.y != 0 ? 1 : 2); }
        int FootprintWallCells(const Portal& p, glm::ivec3 out[6]) {
            const glm::ivec3 n = glm::ivec3(glm::round(p.normal));
            const glm::ivec3 r = glm::ivec3(glm::round(p.right));
            const glm::ivec3 u = glm::ivec3(glm::round(p.upDir));
            const int na = AxisOf(n), ra = AxisOf(r), ua = AxisOf(u);
            if (na == ra || na == ua || ra == ua) return 0;
            constexpr double kEps = 1e-4;
            const int r0 = static_cast<int>(std::floor(p.origin[ra] - 0.5 + kEps));
            const int r1 = static_cast<int>(std::floor(p.origin[ra] + 0.5 - kEps));
            const int u0 = static_cast<int>(std::floor(p.origin[ua] - 1.0 + kEps));
            const int u1 = static_cast<int>(std::floor(p.origin[ua] + 1.0 - kEps));
            const int nc = static_cast<int>(std::floor(p.origin[na] - 0.5 * n[na]));
            int count = 0;
            for (int a = r0; a <= r1 && count < 6; ++a) {
                for (int b = u0; b <= u1 && count < 6; ++b) {
                    glm::ivec3 c(0);
                    c[ra] = a; c[ua] = b; c[na] = nc;
                    out[count++] = c;
                }
            }
            return count;
        }
        bool FootprintContains(const Portal& p, const glm::ivec3& pos) {
            glm::ivec3 cells[6];
            const int n = FootprintWallCells(p, cells);
            for (int i = 0; i < n; ++i) if (cells[i] == pos) return true;
            return false;
        }
        // Solid full blocks behind the whole footprint, air in front of it.
        // The cell that fails, for the caller's message.
        bool FootprintFits(Game::World* world, const Portal& p, glm::ivec3* failCell = nullptr,
                           bool* failIsWall = nullptr) {
            glm::ivec3 cells[6];
            const int n = FootprintWallCells(p, cells);
            if (n == 0) return false;
            const glm::ivec3 normalI = glm::ivec3(glm::round(p.normal));
            for (int i = 0; i < n; ++i) {
                if (!IsValidPortalSurface(world, cells[i])) {
                    if (failCell) *failCell = cells[i];
                    if (failIsWall) *failIsWall = true;
                    return false;
                }
                if (!IsAirSpace(world, cells[i] + normalI)) {
                    if (failCell) *failCell = cells[i] + normalI;
                    if (failIsWall) *failIsWall = false;
                    return false;
                }
            }
            return true;
        }
        // Two portals on the same wall plane whose rectangles overlap.
        bool PortalsOverlap(const Portal& a, const Portal& b) {
            if (!a.active || !b.active || a.dimension != b.dimension) return false;
            const glm::ivec3 na = glm::ivec3(glm::round(a.normal));
            if (na != glm::ivec3(glm::round(b.normal))) return false;
            const int ax = AxisOf(na);
            if (std::abs(a.origin[ax] - b.origin[ax]) > 0.01) return false;
            auto extent = [](const Portal& p, int axis) {
                return std::abs(static_cast<double>(p.right[axis])) * 0.5 +
                       std::abs(static_cast<double>(p.upDir[axis])) * 1.0;
            };
            for (int axis = 0; axis < 3; ++axis) {
                if (axis == ax) continue;
                const double gap = std::abs(a.origin[axis] - b.origin[axis]);
                if (gap >= extent(a, axis) + extent(b, axis) - 1e-4) return false;
            }
            return true;
        }
        // Keep wallA/wallB (saved, and read by older builds) meaningful for a
        // free portal: the first and last cell of its footprint.
        void SetLegacyWallCells(Portal& p) {
            glm::ivec3 cells[6];
            const int n = FootprintWallCells(p, cells);
            if (n == 0) return;
            p.wallA = cells[0];
            p.wallB = cells[n - 1];
        }

        // /gamerule portal_gun_free_placement (IntegratedServer).
        bool FreePlacementEnabled() {
            return Server::g_integratedServer && Server::g_integratedServer->PortalGunFreePlacement();
        }

        // Free placement's nudge (Portal's UTIL_TestForOrientationVolumes /
        // FindClosestPassableSpace idea, on this engine's voxels): offsets
        // in the surface plane, 1/16 block apart, nearest first, out to half
        // the portal's own size each way — the least move that seats the
        // whole portal on solid backing with air in front.
        const std::vector<glm::dvec2>& NudgeOffsets() {
            static const std::vector<glm::dvec2> offsets = [] {
                std::vector<glm::dvec2> v;
                constexpr double kStep = 1.0 / 16.0;
                // At least the portal's own size each way (±1 across, ±1.5
                // along its length), so a shot near an edge still finds the
                // nearest seat that fits.
                for (int i = -16; i <= 16; ++i) {          // along right: ±1.0
                    for (int j = -24; j <= 24; ++j) {      // along up: ±1.5
                        v.emplace_back(i * kStep, j * kStep);
                    }
                }
                std::stable_sort(v.begin(), v.end(), [](const glm::dvec2& a, const glm::dvec2& b) {
                    return glm::dot(a, a) < glm::dot(b, b);
                });
                return v;
            }();
            return offsets;
        }

        // ── Network broadcast helpers ───────────────────────────────────────
        //
        // Both helpers walk every active session via the global integrated
        // server's session manager and send the packet to each connection
        // (INCLUDING the firing player — the firing client also needs to see
        // its own portals to render them in Phase 4). On a server with no
        // sessions yet (early startup, or feature-on/no-clients), these are
        // safe no-ops.
        // ── Immersive mode ───────────────────────────────────────────────
        //
        // With immersive portals on, a linked pair is not rendered and
        // crossed by the gun's own machinery (PortalRenderer, the client's
        // eye-crossing prediction, Tick below) but by the immersive system:
        // the pair is mirrored into the ImmersivePortalRegistry as a bi-way
        // cluster of two oval surfaces, blue → orange and back, each in its
        // own dimension. That is what makes a blue portal in the Overworld
        // and an orange one in the Nether a working, see-through link. The
        // client's gun manager receives nothing in this mode, so every gun
        // visual it drives (rim, ghost bodies, prediction) stays off.
        bool ImmersiveMode() {
#if ENABLE_IMMERSIVE_PORTALS
            return Game::Portals::ImmersiveNetherPortals() &&
                   Server::g_integratedServer && Server::g_integratedServer->ImmersivePortals();
#else
            return false;
#endif
        }

#if ENABLE_IMMERSIVE_PORTALS
        std::string GunTag(uint64_t gunId) { return "gun:" + std::to_string(gunId); }

        // Drops the gun's surfaces whatever the CURRENT mode says: a surface
        // mirrored while immersive mode was on must still go when the pair
        // closes after the mode changed (or when the rebuild at world open
        // ran against the previous world's mode) — otherwise the client
        // keeps a see-through surface with no rim, the server keeps a
        // crossable portal, and both are saved.
        void RemoveImmersive(uint64_t gunId) {
            auto* registry = Server::g_integratedServer ? Server::g_integratedServer->ImmersivePortals() : nullptr;
            if (!registry) return;
            const std::string tag = GunTag(gunId);
            std::vector<Game::Immersive::PortalId> ids;
            registry->ForEach([&](const Game::Immersive::Portal& p) {
                if (p.tag == tag) ids.push_back(p.id);
            });
            for (auto id : ids) registry->Remove(id);
        }

        // The gun's oval as an immersive surface shape, in the surface's
        // local block units (u across, v up). The SAME hole the client's
        // PortalRenderer cuts — Portal-exact 1 : 1.6875, half-height
        // 0.84375 — so its rim sits exactly on this surface's edge.
        // PortalRenderer's mesh extents are the OUTER border of the rim; the
        // hole edge sits at 1 / (1 + kOuterBorder) = 1 / 1.075 of them.
        //
        // The mask is a 48-gon; INSCRIBED in the hole's ellipse its chords
        // fall 1-2 mm short of the edge the rim shader cuts exactly, and
        // that arc of bare wall showed as a sliver along the rim at some
        // angles. Circumscribe it instead (1 / cos(pi/48)) and reach a
        // little further, under the rim band's innermost flame, which is
        // opaque there (outerMask is 1 at the hole edge).
        constexpr float kOvalMaskOverlap = 1.022f;
        constexpr float kOvalHalfWidth  = 0.5f     / 1.075f * kOvalMaskOverlap;
        constexpr float kOvalHalfHeight = 0.84375f / 1.075f * kOvalMaskOverlap;
        Game::Immersive::PortalShape OvalShape() {
            Game::Immersive::PortalShape shape;
            shape.type = Game::Immersive::PortalShape::Type::Mesh;
            constexpr int kSegments = 48;
            shape.vertices.emplace_back(0.0f, 0.0f);
            for (int i = 0; i < kSegments; ++i) {
                const float a = static_cast<float>(i) / kSegments * 6.28318530718f;
                shape.vertices.emplace_back(kOvalHalfWidth * std::cos(a), kOvalHalfHeight * std::sin(a));
            }
            for (int i = 0; i < kSegments; ++i) {
                shape.indices.push_back(0);
                shape.indices.push_back(static_cast<uint32_t>(1 + i));
                shape.indices.push_back(static_cast<uint32_t>(1 + (i + 1) % kSegments));
            }
            return shape;
        }

        // Rebuild the immersive cluster for a gun from its pair: none unless
        // both ends are open.
        void SyncImmersive(uint64_t gunId, const PortalPair& pair) {
            RemoveImmersive(gunId);
            if (!pair.blue.active || !pair.orange.active) return;
            auto* registry = Server::g_integratedServer->ImmersivePortals();

            const Portal& b = pair.blue;
            const Portal& o = pair.orange;
            // Entering blue's face (moving along −n_blue) exits orange's face
            // (moving along +n_orange) with up kept up: the rotation takes
            // blue's basis (right, up, n) onto (−right, up, −n) of orange —
            // a proper rotation, since two axes flip.
            const glm::dmat3 A(glm::dvec3(b.right), glm::dvec3(b.upDir), glm::dvec3(b.normal));
            const glm::dmat3 B(-glm::dvec3(o.right), glm::dvec3(o.upDir), -glm::dvec3(o.normal));
            const glm::dquat rotation = glm::normalize(glm::quat_cast(B * glm::transpose(A)));

            Game::Immersive::Portal front;
            front.kind          = Game::Immersive::PortalKind::PortalGun;
            front.flags         = Game::Immersive::PortalFlag::Default;
            front.dimension     = b.dimension;
            front.origin        = b.origin;
            front.axisW         = glm::dvec3(b.right);
            front.axisH         = glm::dvec3(b.upDir);
            front.width         = 2.0 * kOvalHalfWidth;
            front.height        = 2.0 * kOvalHalfHeight;
            front.destDimension = o.dimension;
            front.destination   = o.origin;
            front.rotation      = rotation;
            front.scale         = 1.0;
            front.shape         = OvalShape();
            front.tag           = GunTag(gunId);
            const auto id = registry->AddBiWay(front);
            if (id == Game::Immersive::kInvalidPortalId) {
                Log::Warning("[PortalGun] Could not mirror gun=%llu into the immersive registry",
                             static_cast<unsigned long long>(gunId));
            }
        }
#endif

        Network::PortalSetS2CPacket BuildSetPacket(uint64_t gunId,
                                                   PortalColor color,
                                                   const Portal& p,
                                                   bool opening = false) {
            Network::PortalSetS2CPacket pk;
            // Only a real placement (a shot, /portalgun move) plays the
            // opening; a catch-up sync shows the portal already open.
            pk.opening = opening ? 1 : 0;
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
            pk.immersive = ImmersiveMode() ? 1 : 0;
            pk.dimensionId = static_cast<int8_t>(Game::DimensionToRaw(p.dimension));
            return pk;
        }

        void BroadcastPortalSet(uint64_t gunId, PortalColor color, const Portal& p) {
            if (!Server::g_integratedServer) return;
            auto* mgr = Server::g_integratedServer->GetSessionManager();
            if (!mgr) return;
            // Every broadcast set is a portal opening (PlacePortal,
            // MovePortals); SyncToClient builds its own, not opening.
            const auto packet = BuildSetPacket(gunId, color, p, /*opening=*/true);
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

    void PortalRegistry::NotePlayerName(const std::string& name) {
        if (name.empty()) return;
        m_playerNames[Game::Anvil::UuidToString(Game::Anvil::OfflinePlayerUuid(name))] = name;
    }

    std::string PortalRegistry::PlayerNameFor(const std::string& uuidOrName) const {
        auto it = m_playerNames.find(uuidOrName);
        return it != m_playerNames.end() ? it->second : uuidOrName;
    }

    PortalPair* PortalRegistry::TryGetPairMutable(uint64_t gunId) {
        auto it = m_pairs.find(gunId);
        return (it == m_pairs.end()) ? nullptr : &it->second;
    }

    void PortalRegistry::ClearPair(uint64_t gunId) {
        auto it = m_pairs.find(gunId);
        if (it == m_pairs.end()) return;
#if ENABLE_IMMERSIVE_PORTALS
        RemoveImmersive(gunId);   // not gated on ImmersiveMode — see RemoveImmersive
#endif
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

    namespace {
        std::string GunSavePath() {
            if (!Server::g_integratedServer) return {};
            Server::ServerLevel* overworld = Server::g_integratedServer->GetLevel(Game::DimensionId::Overworld);
            if (!overworld) return {};
            const std::string& root = overworld->Config().savePath;
            if (root.empty()) return {};
            return (std::filesystem::path(root) / "data" / "portal_gun.json").string();
        }
        nlohmann::json PortalToJson(const Portal& p) {
            return {
                {"active", p.active},
                {"dimension", Game::DimensionToRaw(p.dimension)},
                {"origin", {p.origin.x, p.origin.y, p.origin.z}},
                {"normal", {p.normal.x, p.normal.y, p.normal.z}},
                {"up",     {p.upDir.x, p.upDir.y, p.upDir.z}},
                {"right",  {p.right.x, p.right.y, p.right.z}},
                {"wallA",  {p.wallA.x, p.wallA.y, p.wallA.z}},
                {"wallB",  {p.wallB.x, p.wallB.y, p.wallB.z}},
            };
        }
        // The gun's last confirmed location. The session handle (entityId)
        // means nothing after a relaunch, so a dropped item or an entity
        // holder comes back as Stored at its last position — on disk with
        // that chunk, which is exactly where the save put it.
        nlohmann::json WhereaboutsToJson(const GunWhereabouts& w) {
            nlohmann::json j = {
                {"kind", static_cast<int>(w.kind)},
                {"dimension", Game::DimensionToRaw(w.dimension)},
                {"pos", {w.pos.x, w.pos.y, w.pos.z}},
            };
            if (!w.player.empty()) j["player"] = w.player;
            return j;
        }
        GunWhereabouts WhereaboutsFromJson(const nlohmann::json& j) {
            GunWhereabouts w;
            if (!j.is_object()) return w;
            const int kind = j.value("kind", 0);
            w.dimension = Game::DimensionFromRaw(j.value("dimension", 0));
            if (j.contains("pos") && j["pos"].is_array() && j["pos"].size() == 3) {
                w.pos = {j["pos"][0].get<int>(), j["pos"][1].get<int>(), j["pos"][2].get<int>()};
            }
            w.player = j.value("player", std::string{});
            switch (static_cast<GunWhereabouts::Kind>(kind)) {
                case GunWhereabouts::Kind::Player:
                    w.kind = w.player.empty() ? GunWhereabouts::Kind::Unknown : GunWhereabouts::Kind::Player;
                    break;
                case GunWhereabouts::Kind::Container:
                    w.kind = GunWhereabouts::Kind::Container;
                    break;
                case GunWhereabouts::Kind::ItemEntity:
                case GunWhereabouts::Kind::Entity:
                case GunWhereabouts::Kind::Stored:
                    w.kind = GunWhereabouts::Kind::Stored;
                    break;
                default:
                    w.kind = GunWhereabouts::Kind::Unknown;
                    break;
            }
            return w;
        }
        bool PortalFromJson(const nlohmann::json& j, Portal& p) {
            auto v3 = [&](const char* key, auto& out) {
                if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) return false;
                out = {j[key][0].get<double>(), j[key][1].get<double>(), j[key][2].get<double>()};
                return true;
            };
            glm::dvec3 o, n, u, r, a, b;
            if (!v3("origin", o) || !v3("normal", n) || !v3("up", u) || !v3("right", r) ||
                !v3("wallA", a) || !v3("wallB", b)) return false;
            p.origin = o; p.normal = glm::vec3(n); p.upDir = glm::vec3(u); p.right = glm::vec3(r);
            p.wallA = glm::ivec3(a); p.wallB = glm::ivec3(b);
            p.active = j.value("active", false);
            p.dimension = Game::DimensionFromRaw(j.value("dimension", 0));
            return true;
        }
    }

    bool PortalRegistry::Save() const {
        const std::string path = GunSavePath();
        if (path.empty()) return false;
        try {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            nlohmann::json j;
            j["version"] = 2;
            j["nextId"]  = m_nextId;
            // 1 = the one-time orphan sweep has run for this world (see
            // PortalGunTracker.cpp); pairs from then on are tracked live.
            j["orphanSweep"] = m_orphanSweepDone ? 1 : 0;
            nlohmann::json pairs = nlohmann::json::array();
            for (const auto& [gunId, pair] : m_pairs) {
                nlohmann::json pj = {{"gun", gunId},
                                     {"blue", PortalToJson(pair.blue)},
                                     {"orange", PortalToJson(pair.orange)},
                                     {"seen", WhereaboutsToJson(pair.seen)}};
                if (!pair.owner.empty()) pj["owner"] = pair.owner;
                pairs.push_back(std::move(pj));
            }
            j["pairs"] = std::move(pairs);
            nlohmann::json players = nlohmann::json::object();
            for (const auto& [uuid, name] : m_playerNames) players[uuid] = name;
            j["players"] = std::move(players);
            const std::string tmp = path + ".tmp";
            {
                std::ofstream f(tmp, std::ios::trunc);
                if (!f) return false;
                f << j.dump(2);
            }
            std::filesystem::rename(tmp, path, ec);
            return !ec;
        } catch (const std::exception& e) {
            Log::Warning("[PortalGun] Could not write %s: %s", path.c_str(), e.what());
            return false;
        }
    }

    bool PortalRegistry::Load() {
        // The registry is process-global and outlives a world: whatever the
        // previous world left behind must not leak into this one, even when
        // this world has no file of its own.
        m_pairs.clear();
        m_prevWaist.clear();
        m_prevPlayerPos.clear();
        m_teleportCooldown.clear();
        m_orphanSweepDone = false;
        m_playerNames.clear();
        const std::string path = GunSavePath();
        if (path.empty()) return false;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return false;
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;
            m_pairs.clear();
            if (j.contains("pairs") && j["pairs"].is_array()) {
                for (const auto& pj : j["pairs"]) {
                    const uint64_t gunId = pj.value("gun", uint64_t{0});
                    if (gunId == 0) continue;
                    PortalPair pair;
                    if (pj.contains("blue"))   PortalFromJson(pj["blue"], pair.blue);
                    if (pj.contains("orange")) PortalFromJson(pj["orange"], pair.orange);
                    if (!pair.blue.active && !pair.orange.active) continue;
                    pair.owner = pj.value("owner", std::string{});
                    if (pj.contains("seen")) pair.seen = WhereaboutsFromJson(pj["seen"]);
                    m_pairs[gunId] = pair;
                    m_nextId = std::max(m_nextId, gunId + 1);
                }
            }
            m_nextId = std::max(m_nextId, j.value("nextId", uint64_t{1}));
            m_orphanSweepDone = j.value("orphanSweep", 0) >= 1;
            if (j.contains("players") && j["players"].is_object()) {
                for (const auto& [uuid, name] : j["players"].items()) {
                    if (name.is_string()) m_playerNames[uuid] = name.get<std::string>();
                }
            }
            Log::Info("[PortalGun] Loaded %zu pair(s) from %s", m_pairs.size(), path.c_str());
            return true;
        } catch (const std::exception& e) {
            Log::Warning("[PortalGun] Could not read %s: %s", path.c_str(), e.what());
            return false;
        }
    }

    void PortalRegistry::OnImmersiveCrossing(const std::string& tag) {
        constexpr const char* kPrefix = "gun:";
        if (tag.rfind(kPrefix, 0) != 0) return;
        uint64_t gunId = 0;
        try { gunId = std::stoull(tag.substr(4)); } catch (...) { return; }
        if (m_pairs.find(gunId) == m_pairs.end()) return;
        BroadcastTeleportFlash(gunId);
    }

    void PortalRegistry::RebuildImmersive() {
#if ENABLE_IMMERSIVE_PORTALS
        if (!ImmersiveMode()) return;
        for (const auto& [gunId, pair] : m_pairs) SyncImmersive(gunId, pair);
#endif
    }

    void PortalRegistry::OnBlockChanged(Game::DimensionId dimension, const glm::ivec3& pos) {
        // Walk every pair and drop any portal whose wallA or wallB
        // matches the changed block. Per-color drop so an orphaned
        // sibling on a different wall keeps rendering as an inactive
        // portal.
        for (auto it = m_pairs.begin(); it != m_pairs.end(); ) {
            const uint64_t gunId = it->first;
            PortalPair& pair = it->second;

            auto destroy = [&](Portal& portal, PortalColor color) {
                if (!portal.active) return;
                if (portal.dimension != dimension) return;
                // Any wall cell behind the opening (2 on the grid, up to 6
                // for a free-placed portal).
                if (!FootprintContains(portal, pos)) return;
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

            const bool wasLinked = pair.blue.active && pair.orange.active;
            destroy(pair.blue,   PortalColor::Blue);
            destroy(pair.orange, PortalColor::Orange);
#if ENABLE_IMMERSIVE_PORTALS
            if (wasLinked && !(pair.blue.active && pair.orange.active)) {
                RemoveImmersive(gunId);
            }
#endif

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
                                            Server::ServerPlayer* player,
                                            float facingYaw) {
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
        // orientation is fixed (Portal 1: a wall portal is always upright,
        // a floor or ceiling portal's up is the way the player faces, so
        // you come out of it facing the way you aimed). What varies is the
        // ANCHOR: the hit block is tried as the bottom cell first, then as
        // the top cell — the portal is bumped along its own axis into the
        // spot that fits, never rotated. The first candidate that fits wins.
        std::vector<PortalCandidate> candidates;
        candidates.reserve(2);

        glm::ivec3 upI{0, 1, 0};
        if (IsVerticalWall(hit.face)) {
            upI = glm::ivec3(0, 1, 0);
        } else if (IsFloor(hit.face) || IsCeiling(hit.face)) {
            // The player's primary facing snapped to NESW...
            const float yaw = !std::isnan(facingYaw) ? facingYaw
                            : (player ? player->getYaw() : 0.0f);
            upI = PlayerFacingHorizontal(yaw);
            // ...unless the other portal of this gun is a floor or ceiling
            // portal too: then this one takes ITS facing. The link maps one
            // portal's up onto the other's, so two flat portals facing the
            // same way carry a player straight through with the heading
            // kept; facing opposite ways they spun the player 180° on every
            // trip (user request: no flip, a smooth fall-in/fly-out).
            if (auto pairIt = m_pairs.find(gunId); pairIt != m_pairs.end()) {
                const Portal& partner = color == PortalColor::Blue ? pairIt->second.orange : pairIt->second.blue;
                if (partner.active && std::abs(partner.normal.y) > 0.5f) {
                    const glm::ivec3 partnerUp = glm::ivec3(glm::round(partner.upDir));
                    if (partnerUp != glm::ivec3(0) && partnerUp.y == 0) upI = partnerUp;
                }
            }
        } else {
            Log::Info("[PortalGun] Fizzle: invalid face %d", hit.face);
            BroadcastPortalFizzle(glm::dvec3(hit.hitPoint),
                                  glm::vec3(normalI),
                                  static_cast<uint8_t>(color),
                                  kFizzleBadSurface);
            return PlaceResult::Fizzled;
        }
        Portal p;
        p.dimension = world->GetDimension();
        const char* placementLabel = "free";
        bool placedFree = false;
        if (FreePlacementEnabled()) {
            // /gamerule portal_gun_free_placement: the portal's centre is
            // the exact hit point on the face, then nudged as little as
            // needed to fit (NudgeOffsets) — same orientation rules as the
            // grid, no snapping. Never onto its partner.
            p.normal = glm::vec3(normalI);
            p.upDir  = glm::vec3(upI);
            p.right  = glm::cross(p.upDir, p.normal);
            p.active = true;
            const int na = AxisOf(normalI);
            glm::dvec3 onFace = hit.hitPoint;
            onFace[na] = static_cast<double>(hit.blockPos[na] + (normalI[na] > 0 ? 1 : 0));
            const Portal* partner = nullptr;
            if (auto pairIt = m_pairs.find(gunId); pairIt != m_pairs.end()) {
                const Portal& other = color == PortalColor::Blue ? pairIt->second.orange : pairIt->second.blue;
                if (other.active) partner = &other;
            }
            bool fitted = false;
            for (const glm::dvec2& off : NudgeOffsets()) {
                p.origin = onFace + glm::dvec3(p.right) * off.x + glm::dvec3(p.upDir) * off.y +
                           glm::dvec3(p.normal) * kSurfaceOffset;
                if (!FootprintFits(world, p)) continue;
                if (partner && PortalsOverlap(p, *partner)) continue;
                fitted = true;
                break;
            }
            if (fitted) {
                SetLegacyWallCells(p);
                placedFree = true;
            } else {
                // Nothing off the grid fits (an exactly 1×2 recess, a
                // wall's edge): the grid placement below, exactly as with
                // the rule off, and only if THAT fails too does it fizzle.
                Log::Info("[PortalGun] no free placement fits near (%.2f,%.2f,%.2f) face=%d - trying the grid",
                          hit.hitPoint.x, hit.hitPoint.y, hit.hitPoint.z, hit.face);
            }
        }
        if (!placedFree) {
        candidates.push_back(MakeCandidate(hit.blockPos,       normalI, upI, "hit-is-bottom"));
        candidates.push_back(MakeCandidate(hit.blockPos - upI, normalI, upI, "hit-is-top (bumped)"));

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

        p.origin = chosen->origin;
        p.normal = chosen->normal;
        p.upDir  = chosen->upDir;
        p.right  = chosen->right;
        p.wallA  = chosen->wallA;
        p.wallB  = chosen->wallB;
        p.active = true;
        placementLabel = chosen->label;
        }

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
        // The shooter owns the pair now, and is holding the gun: the gun
        // tracker starts from there (PortalGunTracker.cpp).
        if (player) {
            pair.owner = player->getName();
            pair.seen = GunWhereabouts{};
            pair.seen.kind      = GunWhereabouts::Kind::Player;
            pair.seen.player    = player->getName();
            pair.seen.dimension = p.dimension;
        }

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
                  placementLabel);

        // Broadcast the placement (or move) to every connected client. The
        // firing client also receives this — it's how the local renderer
        // (Phase 4) finds out where to draw the portal.
#if ENABLE_IMMERSIVE_PORTALS
        // The immersive surfaces carry the see-through and the crossing;
        // the pair still goes to the client (flagged immersive) for the
        // gun's own rim.
        if (ImmersiveMode()) SyncImmersive(gunId, pair);
#endif
        BroadcastPortalSet(gunId, color, p);
        return PlaceResult::Placed;
    }

    bool PortalRegistry::MovePortals(uint64_t gunId, bool moveBlue, const glm::ivec3& blueDelta,
                                     bool moveOrange, const glm::ivec3& orangeDelta,
                                     bool force, std::string& error) {
        auto it = m_pairs.find(gunId);
        if (it == m_pairs.end()) { error = "that gun has no open portals"; return false; }
        PortalPair& pair = it->second;
        if (!moveBlue && !moveOrange) { error = "nothing to move"; return false; }
        if (moveBlue && !pair.blue.active)     { error = "its blue portal is not open"; return false; }
        if (moveOrange && !pair.orange.active) { error = "its orange portal is not open"; return false; }

        auto moved = [](const Portal& p, const glm::ivec3& d) {
            Portal q = p;
            q.origin += glm::dvec3(d);
            q.wallA  += d;
            q.wallB  += d;
            return q;
        };
        const Portal newBlue   = moveBlue   ? moved(pair.blue,   blueDelta)   : pair.blue;
        const Portal newOrange = moveOrange ? moved(pair.orange, orangeDelta) : pair.orange;

        if (!force) {
            char buf[192];
            auto validate = [&](const Portal& p, const char* name) -> bool {
                Server::ServerLevel* level = Server::g_integratedServer
                    ? Server::g_integratedServer->GetLevel(p.dimension) : nullptr;
                Game::World* world = level ? level->World() : nullptr;
                if (!world) {
                    error = std::string("the ") + name + " portal's dimension is not loaded";
                    return false;
                }
                // The whole footprint: 2 cells on the grid, up to 6 for a
                // free-placed portal (offsets keep it off-grid; no re-snap).
                glm::ivec3 cell(0);
                bool isWall = true;
                if (!FootprintFits(world, p, &cell, &isWall)) {
                    std::snprintf(buf, sizeof(buf),
                                  isWall ? "the %s portal would need a solid full block at %d %d %d behind it"
                                         : "the %s portal would need free space at %d %d %d in front of it",
                                  name, cell.x, cell.y, cell.z);
                    error = buf;
                    return false;
                }
                return true;
            };
            if (moveBlue && !validate(newBlue, "blue")) return false;
            if (moveOrange && !validate(newOrange, "orange")) return false;
            // Two portals on one wall cell (same wall, same side) would be
            // one opening leading into itself.
            if (PortalsOverlap(newBlue, newOrange)) {
                error = "the blue and orange portals would overlap";
                return false;
            }
        }

        // Commit — the same steps a re-fire takes in PlacePortal.
#if ENABLE_IMMERSIVE_PORTALS
        RemoveImmersive(gunId);   // whatever the mode (see RemoveImmersive)
#endif
        auto commit = [&](Portal& slot, const Portal& next, PortalColor color) {
            BroadcastPortalFizzle(slot.origin, slot.normal, static_cast<uint8_t>(color), kFizzleClose);
            slot = next;
            const uint64_t key = PortalKey(gunId, color);
            for (auto& [pid, m] : m_prevWaist) m.erase(key);
            BroadcastPortalSet(gunId, color, slot);
            Log::Info("[PortalGun] Moved %s portal of gun=%llu to (%.2f,%.2f,%.2f)%s",
                      color == PortalColor::Blue ? "BLUE" : "ORANGE",
                      static_cast<unsigned long long>(gunId),
                      slot.origin.x, slot.origin.y, slot.origin.z, force ? " (forced)" : "");
        };
        if (moveBlue)   commit(pair.blue,   newBlue,   PortalColor::Blue);
        if (moveOrange) commit(pair.orange, newOrange, PortalColor::Orange);
#if ENABLE_IMMERSIVE_PORTALS
        if (ImmersiveMode()) SyncImmersive(gunId, pair);
#endif
        Save();
        return true;
    }

    void PortalRegistry::Tick(Server::IntegratedServer* server) {
        if (!server || m_pairs.empty()) return;
        // Immersive mode: crossings are the immersive traveler's (client) and
        // EntityPortalTravel's (server); this contact test would double up.
        if (ImmersiveMode()) return;

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
                    // A rider goes through with its vehicle (the mob crossing),
                    // never on its own.
                    if (player->isPassenger()) continue;

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
                    const glm::dvec3 fwd = glm::dvec3(Game::Mth::ViewVector(
                        player->getPitch(), player->getYaw()));
                    const glm::dvec3 newFwd =
                        glm::normalize(glm::dmat3(M) * fwd);

                    float newPitchDeg =
                        Game::Mth::XRotFromVector(glm::vec3(newFwd));
                    newPitchDeg = std::clamp(newPitchDeg, -89.5f, 89.5f);

                    float newYawDeg;
                    const double horizLen2 =
                        newFwd.x * newFwd.x + newFwd.z * newFwd.z;
                    if (horizLen2 > 0.01) {
                        newYawDeg = Game::Mth::YRotFromVector(glm::vec3(newFwd));
                    } else if (std::abs(s.other->normal.y) > 0.7f) {
                        // Floor/ceiling exit — align with -upDir.
                        newYawDeg = Game::Mth::YRotFromVector(-s.other->upDir);
                    } else {
                        // Wall dst with degenerate horizontal forward
                        // — recover yaw from M-rotated RIGHT vector
                        // (matches client; see ClientPortalManager.cpp).
                        const glm::dvec3 rightH = glm::normalize(glm::cross(
                            glm::dvec3(Game::Mth::HorizontalViewVector(player->getYaw())),
                            glm::dvec3(0.0, 1.0, 0.0)));
                        const glm::dvec3 newRight = glm::normalize(
                            glm::dmat3(M) * rightH);
                        newYawDeg = Game::Mth::YRotFromVector(glm::vec3(
                            glm::cross(glm::dvec3(0.0, 1.0, 0.0), newRight)));
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
