// File: src/common/entity/PortalWandBehavior.cpp
//
// The portal wand: an immersive portal drawn with four clicks, the
// Immersive Portals mod's wand reduced to what a click on a block face
// can say.
//
//   1. Right-click a block: one corner of the NEAR surface. The face you
//      click is the wall the surface sits on.
//   2. Right-click the opposite corner on the same face: the near surface
//      is the rectangle of cells between the two clicks, lying on that
//      face, one block in front of the wall.
//   3–4. The same for the FAR surface, anywhere — another wall, the floor,
//      another dimension.
//   Shift + right-click at any point starts over.
//
// The far rectangle sets the scale (its width over the near width) and the
// rotation (the near face's frame turned onto the far face's, up staying
// up), and the pair is made two-way, two-faced — the same records as
// `/portal make_full`, so `/portal remove` next to either end takes it.
#include "../core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "Item.hpp"
#include "../core/Log.hpp"
#include "../world/block/BlockInteraction.hpp"
#include "../world/level/DimensionId.hpp"
#include "../portal/ImmersivePortal.hpp"

#include "server/IntegratedServer.hpp"
#include "server/portal/ImmersivePortalRegistry.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/network/ServerConnection.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace Game::Portal {

    namespace {
        // Face ordinals of BlockHitResult: 0 bottom, 1 top, 2 north, 3 south,
        // 4 west, 5 east.
        glm::ivec3 FaceNormal(int face) {
            switch (face) {
                case 0: return { 0, -1,  0};
                case 1: return { 0,  1,  0};
                case 2: return { 0,  0, -1};
                case 3: return { 0,  0,  1};
                case 4: return {-1,  0,  0};
                default: return { 1,  0,  0};
            }
        }
        int NormalAxis(const glm::ivec3& n) { return n.x != 0 ? 0 : (n.y != 0 ? 1 : 2); }

        struct Corner {
            glm::ivec3 cell{0};
            int        face = 1;
            Game::DimensionId dimension = Game::DimensionId::Overworld;
        };

        // A finished rectangle of cells on one face plane.
        struct Surface {
            glm::dvec3 origin{0.0};   // centre, on the face plane
            glm::dvec3 axisW{1.0, 0.0, 0.0};
            glm::dvec3 axisH{0.0, 1.0, 0.0};
            glm::dvec3 normal{0.0, 0.0, 1.0};
            double     width  = 1.0;
            double     height = 1.0;
            Game::DimensionId dimension = Game::DimensionId::Overworld;
        };

        struct WandState {
            int    clicks = 0;      // corners recorded so far, 0..3
            Corner corner[4];
        };
        std::unordered_map<uint32_t, WandState> g_wands;   // by player id

        void Say(uint32_t playerId, const std::string& text) {
            if (!Server::g_integratedServer) return;
            auto* sessions = Server::g_integratedServer->GetSessionManager();
            if (!sessions) return;
            if (auto session = sessions->GetSession(playerId)) {
                if (auto* conn = session->GetConnection()) conn->SendChatMessage(text, 1);
            }
        }

        // Two corners on one face into a surface. False with a reason when
        // they do not describe one.
        bool BuildSurface(const Corner& a, const Corner& b, Surface& out, std::string& why) {
            if (a.dimension != b.dimension) { why = "Both corners must be in the same dimension"; return false; }
            if (a.face != b.face) { why = "Both corners must be on the same face (the same wall, floor or ceiling)"; return false; }
            const glm::ivec3 n = FaceNormal(a.face);
            const int na = NormalAxis(n);
            if (a.cell[na] != b.cell[na]) { why = "Both corners must be on the same plane"; return false; }

            const glm::ivec3 lo = glm::min(a.cell, b.cell);
            const glm::ivec3 hi = glm::max(a.cell, b.cell);
            // The surface lies on the clicked face: the plane between the
            // clicked cell and the cell in front of it.
            const double planeCoord = static_cast<double>(a.cell[na]) + (n[na] > 0 ? 1.0 : 0.0);

            // In-plane axes. A wall: W across, H up. A floor or ceiling: W
            // along +X, H along ∓Z so that cross(W, H) is the face normal.
            glm::dvec3 axisW, axisH;
            const glm::dvec3 nd(n);
            if (n.y == 0) {
                axisH = glm::dvec3(0.0, 1.0, 0.0);
                axisW = glm::cross(axisH, nd);
            } else {
                axisW = glm::dvec3(1.0, 0.0, 0.0);
                axisH = glm::dvec3(0.0, 0.0, n.y > 0 ? -1.0 : 1.0);
            }
            auto extentAlong = [&](const glm::dvec3& axis) {
                const int ax = std::abs(axis.x) > 0.5 ? 0 : (std::abs(axis.y) > 0.5 ? 1 : 2);
                return static_cast<double>(hi[ax] - lo[ax] + 1);
            };
            out.axisW  = axisW;
            out.axisH  = axisH;
            out.normal = nd;
            out.width  = extentAlong(axisW);
            out.height = extentAlong(axisH);
            glm::dvec3 centre = (glm::dvec3(lo) + glm::dvec3(hi) + glm::dvec3(1.0)) * 0.5;
            centre[na] = planeCoord;
            out.origin = centre;
            out.dimension = a.dimension;
            return true;
        }
    } // namespace

    UseResult OnWandUseOn(const UseOnContext& ctx, ItemStack& /*stack*/) {
        if (!ctx.world || !ctx.player) return UseResult::Pass;
        auto* player = static_cast<Server::ServerPlayer*>(ctx.player);
        const uint32_t playerId = player->getPlayerId();
        WandState& state = g_wands[playerId];

        if (ctx.player->IsSneaking()) {
            state = WandState{};
            Say(playerId, "Wand reset. Click the first corner of the near surface.");
            return UseResult::Success;
        }
        if (!Server::g_integratedServer || !Server::g_integratedServer->ImmersivePortals()) {
            Say(playerId, "Immersive portals are not available");
            return UseResult::Fail;
        }

        Corner c;
        c.cell      = ctx.hitResult.blockPos;
        c.face      = ctx.hitResult.face;
        c.dimension = Game::DimensionFromRaw(player->getDimensionId());
        state.corner[state.clicks] = c;

        // A surface is checked as soon as its second corner lands, so a bad
        // pair is reported at once and the click is not counted.
        if (state.clicks == 1 || state.clicks == 3) {
            Surface probe; std::string why;
            if (!BuildSurface(state.corner[state.clicks - 1], c, probe, why)) {
                Say(playerId, why + ". Click that corner again.");
                return UseResult::Fail;
            }
        }
        ++state.clicks;

        char buf[160];
        switch (state.clicks) {
            case 1:
                Say(playerId, "Near corner 1 set. Click the opposite corner on the same face.");
                return UseResult::Success;
            case 2: {
                Surface s; std::string why; BuildSurface(state.corner[0], state.corner[1], s, why);
                std::snprintf(buf, sizeof(buf), "Near surface %.0fx%.0f. Now click the far surface's two corners.",
                              s.width, s.height);
                Say(playerId, buf);
                return UseResult::Success;
            }
            case 3:
                Say(playerId, "Far corner 1 set. Click the opposite corner on the same face.");
                return UseResult::Success;
            default:
                break;
        }

        // Four corners: make the pair.
        Surface nearS, farS; std::string why;
        BuildSurface(state.corner[0], state.corner[1], nearS, why);
        BuildSurface(state.corner[2], state.corner[3], farS, why);
        state = WandState{};

        const double scaleW = farS.width  / nearS.width;
        const double scaleH = farS.height / nearS.height;
        if (std::abs(scaleW - scaleH) > 0.1 * std::max(scaleW, scaleH)) {
            std::snprintf(buf, sizeof(buf),
                          "The far surface (%.0fx%.0f) must have the near surface's proportions (%.0fx%.0f). Start over.",
                          farS.width, farS.height, nearS.width, nearS.height);
            Say(playerId, buf);
            return UseResult::Fail;
        }

        Game::Immersive::Portal portal;
        portal.dimension     = nearS.dimension;
        portal.destDimension = farS.dimension;
        portal.origin        = nearS.origin;
        portal.axisW         = nearS.axisW;
        portal.axisH         = nearS.axisH;
        portal.width         = nearS.width;
        portal.height        = nearS.height;
        portal.destination   = farS.origin;
        portal.scale         = scaleW;
        portal.tag           = "wand";
        // The rotation that turns the near frame, seen from behind, onto the
        // far frame: the reverse record then faces the far face's normal
        // with its width along the far rectangle (MakeReverse gives it
        // R·(−W), R·H, R·(−N)).
        {
            const glm::dmat3 nearFrame(-nearS.axisW, nearS.axisH, -nearS.normal);   // columns
            const glm::dmat3 farFrame(farS.axisW, farS.axisH, farS.normal);
            const glm::dmat3 R = farFrame * glm::transpose(nearFrame);
            portal.rotation = glm::normalize(glm::quat_cast(R));
        }

        auto& registry = *Server::g_integratedServer->ImmersivePortals();
        const auto id = registry.AddBiWayBiFaced(portal);
        if (id == Game::Immersive::kInvalidPortalId) {
            Say(playerId, "Could not create the portal (invalid geometry). Start over.");
            return UseResult::Fail;
        }
        std::snprintf(buf, sizeof(buf), "Created portal #%u: %.0fx%.0f -> %.0fx%.0f, scale %.2f",
                      static_cast<unsigned>(id), nearS.width, nearS.height, farS.width, farS.height, scaleW);
        Say(playerId, buf);
        Log::Info("[PortalWand] %s created portal #%u", player->getName().c_str(), static_cast<unsigned>(id));
        return UseResult::Success;
    }

} // namespace Game::Portal

#endif // ENABLE_IMMERSIVE_PORTALS
