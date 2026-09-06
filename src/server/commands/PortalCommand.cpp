// File: src/server/commands/PortalCommand.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "PortalCommand.hpp"
#include "CommandCoords.hpp"
#include "EntitySelector.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../entity/MobManager.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "common/entity/Mob.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../portal/ImmersivePortalRegistry.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Server {

    namespace {

        using Game::Immersive::Portal;
        using Game::Immersive::PortalId;
        using Game::Immersive::kInvalidPortalId;

        constexpr const char* kUsage =
            "Usage: /portal make|make_biway|make_full <w> <h> <dim> <x> <y> <z> | "
            "make_loop <w> <h> <dx> <dy> <dz> [turn degrees] | make_mirror <w> <h> | "
            "set_rotation <ax> <ay> <az> <degrees> | set_scale <scale> | "
            "list | info | remove [id] | remove_all";
        constexpr double kMinScale = 0.1;
        constexpr double kMaxScale = 32.0;

        // Portal sizes the frame math and the renderer are happy with.
        constexpr double kMinSize = 0.25;
        constexpr double kMaxSize = 64.0;
        // How far in front of the player a new portal stands.
        constexpr double kPlacementDistance = 1.5;
        // /portal info reach.
        constexpr double kInfoRadius = 32.0;

        bool ParseDimension(const std::string& s, Game::DimensionId& out) {
            std::string k = s;
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return std::tolower(c); });
            if (k == "overworld" || k == "0")                        { out = Game::DimensionId::Overworld; return true; }
            if (k == "nether" || k == "the_nether" || k == "-1")     { out = Game::DimensionId::Nether;    return true; }
            if (k == "end" || k == "the_end" || k == "1")            { out = Game::DimensionId::End;       return true; }
            return false;
        }

        bool ParseSize(const std::string& s, double& out) {
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(v)) return false;
            if (v < kMinSize || v > kMaxSize) return false;
            out = v;
            return true;
        }

        // The surface the player is looking at, facing them: MC yaw 0 is +Z,
        // 90 is −X, so the horizontal forward is (−sin, 0, cos).
        // Move a frame whose axes lie along world axes onto the block grid:
        // its plane on a block boundary, its bottom edge on one, and its side
        // edges on them too (an odd width centres on a block, an even one on
        // a boundary; a fractional width gets one side on the grid). A frame
        // that is not axis-aligned is left alone — there is no grid for it.
        void SnapFrameToGrid(glm::dvec3& origin, const glm::dvec3& axisW, const glm::dvec3& axisH,
                             const glm::dvec3& normal, double width, double height) {
            auto axisIndex = [](const glm::dvec3& v) {
                for (int i = 0; i < 3; ++i) if (std::abs(v[i]) > 0.999) return i;
                return -1;
            };
            const int iw = axisIndex(axisW), ih = axisIndex(axisH), in = axisIndex(normal);
            if (iw < 0 || ih < 0 || in < 0) return;
            origin[in] = std::round(origin[in]);
            origin[iw] = std::round(origin[iw] - width * 0.5)  + width * 0.5;
            origin[ih] = std::round(origin[ih] - height * 0.5) + height * 0.5;
        }

        void FrameFacingPlayer(const ServerPlayer& player, double width, double height,
                               Portal& portal, bool gridAligned = false) {
            double yaw = glm::radians(static_cast<double>(player.getYaw()));
            // A grid-aligned frame faces the nearest world axis.
            if (gridAligned) yaw = std::round(yaw / glm::half_pi<double>()) * glm::half_pi<double>();
            const glm::dvec3 forward(-std::sin(yaw), 0.0, std::cos(yaw));
            const glm::dvec3 normal = -forward;                 // toward the player
            const glm::dvec3 up(0.0, 1.0, 0.0);
            const glm::dvec3 axisW = glm::cross(up, normal);    // so cross(axisW, up) == normal
            portal.axisW  = axisW;
            portal.axisH  = up;
            portal.width  = width;
            portal.height = height;
            portal.origin = player.getPosition() + forward * kPlacementDistance +
                            glm::dvec3(0.0, height * 0.5, 0.0);
            if (gridAligned) SnapFrameToGrid(portal.origin, portal.axisW, portal.axisH, normal, width, height);
        }

        void SendList(ServerConnection& connection, const ImmersivePortalRegistry& registry,
                      Game::DimensionId dimension) {
            size_t n = 0;
            registry.ForEachInDimension(dimension, [&](const Portal& p) {
                connection.SendChatMessage(p.Describe(), 1);
                ++n;
            });
            connection.SendChatMessage(
                std::to_string(n) + " portal(s) in " + std::string(Game::DimensionName(dimension)) +
                " (" + std::to_string(registry.Count()) + " total)", 1);
        }

        bool ParseDouble(const std::string& s, double& out) {
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(v)) return false;
            out = v;
            return true;
        }

        // The portal nearest the sender within /portal info's reach.
        const Portal* NearestPortal(const ImmersivePortalRegistry& registry, const ServerPlayer& sender,
                                    Game::DimensionId here) {
            const Portal* best = nullptr;
            double bestDist = 0.0;
            for (const Portal* p : registry.CollectNear(here, sender.getPosition(), kInfoRadius)) {
                // World-option surfaces are not edited by hand.
                if (p->Has(Game::Immersive::PortalFlag::Global)) continue;
                const double d = glm::length(p->origin - sender.getPosition());
                if (!best || d < bestDist) { best = p; bestDist = d; }
            }
            return best;
        }

        // Write `edited` back and keep its cluster consistent: the flipped
        // twin takes the same rotation and scale, and the reverse of each is
        // rebuilt from it (MakeReverse) under its existing id and links —
        // the mod's `/portal set_portal_rotation` and `set_portal_scale`
        // propagate to the cluster the same way.
        size_t UpdateCluster(ImmersivePortalRegistry& registry, Portal edited) {
            size_t updated = 0;
            auto rebuildReverse = [&](const Portal& front) {
                if (front.reversePortalId == kInvalidPortalId) return;
                const Portal* old = registry.Get(front.reversePortalId);
                if (!old) return;
                Portal r = front.MakeReverse();
                r.id               = old->id;
                r.reversePortalId  = front.id;
                r.flippedPortalId  = old->flippedPortalId;
                r.parallelPortalId = old->parallelPortalId;
                r.tag              = old->tag;
                if (registry.Update(r)) ++updated;
            };
            if (registry.Update(edited)) ++updated;
            rebuildReverse(edited);
            if (edited.flippedPortalId != kInvalidPortalId) {
                if (const Portal* twin = registry.Get(edited.flippedPortalId)) {
                    Portal f = *twin;
                    f.rotation = edited.rotation;
                    f.scale    = edited.scale;
                    if (registry.Update(f)) ++updated;
                    rebuildReverse(f);
                }
            }
            return updated;
        }

    } // namespace

    void PortalCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("portal", PortalCommand::Execute);
        dispatcher.RegisterCommand("scale",  PortalCommand::ExecuteScale);
    }

    void PortalCommand::ExecuteScale(ServerPlayer& sender,
                                     const std::vector<std::string>& args,
                                     ServerConnection& connection,
                                     PlayerSessionManager& sessionManager) {
        // /scale                  → you, back to 1
        // /scale <value>          → you
        // /scale <value> <player> → that player (@s is you)
        // /scale <value> <r>      → every mob and dropped item within r blocks
        double scale = 1.0, radius = 0.0;
        const bool hasRadius = args.size() == 2 && ParseDouble(args[1], radius);
        const bool hasPlayer = args.size() == 2 && !hasRadius;
        const bool bad =
            args.size() > 2 ||
            (args.size() >= 1 && (!ParseDouble(args[0], scale) || scale < kMinScale || scale > kMaxScale)) ||
            (hasRadius && (radius <= 0.0 || radius > 128.0));
        if (bad) {
            connection.SendChatMessage("Usage: /scale [0.1-32] [player|radius] (no value resets you to 1; a name scales that player, a radius scales mobs and items around you)", 1);
            return;
        }
        if (hasPlayer) {
            std::shared_ptr<PlayerSession> target;
            if (args[1] == "@s") {
                target = sessionManager.GetSession(sender.getPlayerId());
            } else {
                std::string want = args[1];
                std::transform(want.begin(), want.end(), want.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                for (const auto& session : sessionManager.GetAllSessions()) {
                    if (!session || !session->GetPlayer()) continue;
                    std::string have = session->GetPlayer()->getName();
                    std::transform(have.begin(), have.end(), have.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (have == want) { target = session; break; }
                }
            }
            if (!target || !target->GetPlayer()) {
                connection.SendChatMessage("No player named '" + args[1] + "'", 1);
                return;
            }
            ServerPlayer& other = *target->GetPlayer();
            other.setScale(static_cast<float>(scale));
            char buf[96];
            if (&other == &sender) {
                connection.SendPlayerAbilities(sender);
                std::snprintf(buf, sizeof(buf), "Your size is now %.2f", sender.getScale());
                connection.SendChatMessage(buf, 1);
                return;
            }
            if (auto* conn = target->GetConnection()) {
                conn->SendPlayerAbilities(other);
                std::snprintf(buf, sizeof(buf), "%s set your size to %.2f", sender.getName().c_str(), other.getScale());
                conn->SendChatMessage(buf, 1);
            }
            std::snprintf(buf, sizeof(buf), "%s's size is now %.2f", other.getName().c_str(), other.getScale());
            connection.SendChatMessage(buf, 1);
            return;
        }
        if (hasRadius) {
            ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(
                Game::DimensionFromRaw(sender.getDimensionId())) : nullptr;
            if (!level) { connection.SendChatMessage("No level", 1); return; }
            const glm::dvec3 here = sender.getPosition();
            size_t mobsDone = 0, itemsDone = 0;
            if (level->Mobs()) {
                for (Game::Mob* mob : level->Mobs()->List()) {
                    if (!mob || glm::length(mob->position - here) > radius) continue;
                    mob->scale = static_cast<float>(scale);
                    mob->needsSync = true;
                    ++mobsDone;
                }
            }
            if (level->Items()) {
                for (auto& [id, item] : level->Items()->AllMutable()) {
                    if (glm::length(item.pos - here) > radius) continue;
                    item.scale = static_cast<float>(scale);
                    item.pendingSpawn = true;
                    item.needsSync    = true;
                    ++itemsDone;
                }
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Scaled %zu mob(s) and %zu item(s) to %.2f",
                          mobsDone, itemsDone, scale);
            connection.SendChatMessage(buf, 1);
            return;
        }
        sender.setScale(static_cast<float>(scale));
        // The abilities packet carries the size to the client.
        connection.SendPlayerAbilities(sender);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Your size is now %.2f", sender.getScale());
        connection.SendChatMessage(buf, 1);
    }

    void PortalCommand::Execute(ServerPlayer& sender,
                                const std::vector<std::string>& args,
                                ServerConnection& connection,
                                PlayerSessionManager& /*sessionManager*/) {
        if (!g_integratedServer || !g_integratedServer->ImmersivePortals()) {
            connection.SendChatMessage("Immersive portals are not available", 1);
            return;
        }
        auto& registry = *g_integratedServer->ImmersivePortals();
        const Game::DimensionId here = Game::DimensionFromRaw(sender.getDimensionId());

        if (args.empty()) {
            connection.SendChatMessage(kUsage, 1);
            return;
        }
        const std::string& sub = args[0];

        if (sub == "list") {
            SendList(connection, registry, here);
            return;
        }

        if (sub == "info") {
            const auto nearby = registry.CollectNear(here, sender.getPosition(), kInfoRadius);
            if (nearby.empty()) {
                connection.SendChatMessage("No portal within " +
                                           std::to_string(static_cast<int>(kInfoRadius)) + " blocks", 1);
                return;
            }
            const Portal* best = nullptr;
            double bestDist = 0.0;
            for (const Portal* p : nearby) {
                const double d = glm::length(p->origin - sender.getPosition());
                if (!best || d < bestDist) { best = p; bestDist = d; }
            }
            connection.SendChatMessage(best->Describe(), 1);
            const glm::dvec3 n = best->Normal();
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "  normal (%.2f, %.2f, %.2f), %.1f blocks away, you are %s it; "
                          "links: reverse #%u flipped #%u parallel #%u",
                          n.x, n.y, n.z, bestDist,
                          best->IsInFront(sender.getPosition()) ? "in front of" : "behind",
                          best->reversePortalId, best->flippedPortalId, best->parallelPortalId);
            connection.SendChatMessage(buf, 1);
            return;
        }

        if (sub == "remove") {
            // `/portal remove` alone takes the nearest portal (a mirror in
            // front of you, say); with an id, that one. Either way the
            // whole cluster goes.
            if (args.size() > 2) { connection.SendChatMessage("Usage: /portal remove [id]", 1); return; }
            PortalId id = Game::Immersive::kInvalidPortalId;
            if (args.size() == 2) {
                char* end = nullptr;
                const unsigned long parsed = std::strtoul(args[1].c_str(), &end, 10);
                if (!end || *end != '\0' || parsed == 0 || !registry.Get(static_cast<PortalId>(parsed))) {
                    connection.SendChatMessage("No portal #" + args[1], 1);
                    return;
                }
                id = static_cast<PortalId>(parsed);
            } else {
                const Portal* target = NearestPortal(registry, sender, here);
                if (!target) { connection.SendChatMessage("No portal nearby", 1); return; }
                id = target->id;
            }
            const size_t removed = registry.RemoveCluster(id);
            connection.SendChatMessage("Removed " + std::to_string(removed) + " portal(s)", 1);
            return;
        }

        if (sub == "make_loop") {
            // A two-way, two-faced portal whose far end is THIS end moved by
            // (dx, dy, dz) and turned by `turn` degrees about vertical — the
            // portal of an endless staircase or corridor: build one segment,
            // stand at its exit facing onward, and give the offset back to
            // its entrance. Coordinates are world axes, the far end faces
            // the way you face, turned.
            if (args.size() != 6 && args.size() != 7) {
                connection.SendChatMessage("Usage: /portal make_loop <w> <h> <dx> <dy> <dz> [turn degrees]", 1);
                return;
            }
            double width = 0.0, height = 0.0;
            if (!ParseSize(args[1], width) || !ParseSize(args[2], height)) {
                connection.SendChatMessage("Width and height must be between 0.25 and 64", 1);
                return;
            }
            glm::dvec3 offset; double turn = 0.0;
            if (!ParseDouble(args[3], offset.x) || !ParseDouble(args[4], offset.y) ||
                !ParseDouble(args[5], offset.z) || (args.size() == 7 && !ParseDouble(args[6], turn))) {
                connection.SendChatMessage("Offset and turn must be numbers", 1);
                return;
            }
            Portal portal;
            portal.dimension     = here;
            portal.destDimension = here;
            portal.tag           = "command";
            // Both ends sit on the block grid. A loop is walked through
            // block corridors: a frame placed at the player's exact
            // sub-block position, turned about its own centre, maps the
            // near corridor's walls a fraction of a block off the far
            // corridor's — a player brushing the near wall arrived inside
            // the far one and stuck there. With the near frame on the grid
            // and the far frame snapped on its own, the transform carries
            // block boundaries onto block boundaries.
            FrameFacingPlayer(sender, width, height, portal, /*gridAligned=*/true);
            portal.destination = portal.origin + offset;
            portal.rotation    = glm::angleAxis(glm::radians(turn), glm::dvec3(0.0, 1.0, 0.0));
            {
                const glm::dvec3 farW = portal.rotation * portal.axisW;
                const glm::dvec3 farH = portal.rotation * portal.axisH;
                const glm::dvec3 farN = portal.rotation * glm::cross(portal.axisW, portal.axisH);
                SnapFrameToGrid(portal.destination, farW, farH, farN, width, height);
            }
            const PortalId id = registry.AddBiWayBiFaced(portal);
            if (id == kInvalidPortalId) {
                connection.SendChatMessage("Could not create the portal (invalid geometry)", 1);
                return;
            }
            const Portal* created = registry.Get(id);
            connection.SendChatMessage("Created loop " + (created ? created->Describe() : std::string("portal")), 1);
            return;
        }

        if (sub == "make_mirror") {
            if (args.size() != 3) { connection.SendChatMessage("Usage: /portal make_mirror <w> <h>", 1); return; }
            double width = 0.0, height = 0.0;
            if (!ParseSize(args[1], width) || !ParseSize(args[2], height)) {
                connection.SendChatMessage("Width and height must be between 0.25 and 64", 1);
                return;
            }
            Portal portal;
            portal.kind          = Game::Immersive::PortalKind::Mirror;
            portal.flags         = Game::Immersive::PortalFlag::Visible |
                                   Game::Immersive::PortalFlag::RenderPlayer |
                                   Game::Immersive::PortalFlag::Mirror;
            portal.dimension     = here;
            portal.destDimension = here;
            portal.tag           = "mirror";
            FrameFacingPlayer(sender, width, height, portal);
            portal.destination   = portal.origin;
            const PortalId id = registry.Add(portal);
            if (id == kInvalidPortalId) {
                connection.SendChatMessage("Could not create the mirror (invalid geometry)", 1);
                return;
            }
            const Portal* created = registry.Get(id);
            connection.SendChatMessage("Created " + (created ? created->Describe() : std::string("mirror")), 1);
            return;
        }

        if (sub == "set_rotation") {
            if (args.size() != 5) {
                connection.SendChatMessage("Usage: /portal set_rotation <ax> <ay> <az> <degrees> (nearest portal)", 1);
                return;
            }
            glm::dvec3 axis; double degrees = 0.0;
            if (!ParseDouble(args[1], axis.x) || !ParseDouble(args[2], axis.y) ||
                !ParseDouble(args[3], axis.z) || !ParseDouble(args[4], degrees) ||
                glm::length(axis) < 1e-6) {
                connection.SendChatMessage("Axis must be a non-zero vector, degrees a number", 1);
                return;
            }
            const Portal* target = NearestPortal(registry, sender, here);
            if (!target) { connection.SendChatMessage("No portal nearby", 1); return; }
            if (target->IsMirror()) { connection.SendChatMessage("A mirror has no rotation", 1); return; }
            if (target->kind == Game::Immersive::PortalKind::NetherPortal) {
                connection.SendChatMessage("A nether portal's surface is its obsidian frame; it cannot be rotated. Use a /portal make_full portal.", 1);
                return;
            }
            Portal edited = *target;
            edited.rotation = glm::angleAxis(glm::radians(degrees), glm::normalize(axis));
            const size_t n = UpdateCluster(registry, edited);
            connection.SendChatMessage("Rotation set on " + std::to_string(n) + " portal(s) of #" +
                                       std::to_string(edited.id), 1);
            return;
        }

        if (sub == "set_scale") {
            if (args.size() != 2) {
                connection.SendChatMessage("Usage: /portal set_scale <scale> (nearest portal)", 1);
                return;
            }
            double scale = 1.0;
            if (!ParseDouble(args[1], scale) || scale < kMinScale || scale > kMaxScale) {
                connection.SendChatMessage("Scale must be between 0.1 and 32", 1);
                return;
            }
            const Portal* target = NearestPortal(registry, sender, here);
            if (!target) { connection.SendChatMessage("No portal nearby", 1); return; }
            if (target->IsMirror()) { connection.SendChatMessage("A mirror has no scale", 1); return; }
            // A nether portal's surfaces ARE its two obsidian frames; a scale
            // set by hand leaves one surface the wrong size for its frame
            // (and the integrity sweep then removes the cluster). Its scale
            // comes from the frames: light a 2x3 with a 4x6 waiting at the
            // far side and the link is made at scale 2.
            if (target->kind == Game::Immersive::PortalKind::NetherPortal) {
                connection.SendChatMessage("A nether portal's scale comes from its frames: build the far frame N times larger (a 2x3 here, a 4x6 there) and light it. Use a /portal make_full portal to scale by command.", 1);
                return;
            }
            Portal edited = *target;
            edited.scale = scale;
            const size_t n = UpdateCluster(registry, edited);
            connection.SendChatMessage("Scale set on " + std::to_string(n) + " portal(s) of #" +
                                       std::to_string(edited.id), 1);
            return;
        }

        if (sub == "remove_all") {
            std::vector<PortalId> ids;
            registry.ForEach([&](const Portal& p) { ids.push_back(p.id); });
            size_t removed = 0;
            for (PortalId id : ids) if (registry.Remove(id)) ++removed;
            connection.SendChatMessage("Removed " + std::to_string(removed) + " portal(s)", 1);
            return;
        }

        const bool make      = sub == "make";
        const bool makeBiWay = sub == "make_biway";
        const bool makeFull  = sub == "make_full";
        if (!(make || makeBiWay || makeFull)) {
            connection.SendChatMessage(kUsage, 1);
            return;
        }
        if (args.size() != 7) {
            connection.SendChatMessage("Usage: /portal " + sub + " <w> <h> <dim> <x> <y> <z>", 1);
            return;
        }

        double width = 0.0, height = 0.0;
        if (!ParseSize(args[1], width) || !ParseSize(args[2], height)) {
            connection.SendChatMessage("Width and height must be between 0.25 and 64", 1);
            return;
        }
        Game::DimensionId destDim;
        if (!ParseDimension(args[3], destDim)) {
            connection.SendChatMessage("Dimension must be overworld, nether or end", 1);
            return;
        }
        CommandSource source;
        source.sender    = &sender;
        source.position  = sender.getPosition();
        source.dimension = here;
        CommandRotation rot{ sender.getYaw(), sender.getPitch() };
        glm::dvec3 dest;
        std::string error;
        if (!ParseVec3(args[4], args[5], args[6], source, rot, dest, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        Portal portal;
        portal.dimension     = here;
        portal.destDimension = destDim;
        portal.destination   = dest;
        portal.tag           = "command";
        FrameFacingPlayer(sender, width, height, portal);

        PortalId id = kInvalidPortalId;
        if (make)           id = registry.Add(portal);
        else if (makeBiWay) id = registry.AddBiWay(portal);
        else                id = registry.AddBiWayBiFaced(portal);

        if (id == kInvalidPortalId) {
            connection.SendChatMessage("Could not create the portal (invalid geometry)", 1);
            return;
        }
        const Portal* created = registry.Get(id);
        connection.SendChatMessage("Created " + (created ? created->Describe() : std::string("portal")), 1);
        Log::Info("[PortalCommand] %s created portal #%u", sender.getName().c_str(), id);
    }

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
