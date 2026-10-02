// File: src/server/commands/PortalGunCommand.cpp
#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalGunCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../portal/PortalGunTracker.hpp"
#include "../portal/PortalRegistry.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace Server {

    namespace {

        constexpr const char* kUsage =
            "Usage: /portalgun list | close <player>|all|gun <id> | sweep | "
            "move <owner>|gun <id>|nearest <blue|orange|both> "
            "<left|right|up|down|forward|back> <blocks> [force] | "
            "move <owner>|gun <id>|nearest <blue|orange|both> offset <dx> <dy> <dz> [force]";
        constexpr const char* kMoveUsage =
            "Usage: /portalgun move <owner>|gun <id>|nearest <blue|orange|both> "
            "<left|right|up|down|forward|back> <blocks> [force]  or  ... offset <dx> <dy> <dz> [force] "
            "(directions as seen facing the portal; forward = into the wall)";
        // /portalgun move nearest: how far from the sender a portal may be.
        constexpr double kNearestRadius = 32.0;

        std::string Lower(std::string s);   // below

        bool ParseInt(const std::string& s, int& out) {
            if (s.empty()) return false;
            char* end = nullptr;
            errno = 0;
            const long v = std::strtol(s.c_str(), &end, 10);
            if (errno != 0 || !end || *end != '\0' || v < -30000000 || v > 30000000) return false;
            out = static_cast<int>(v);
            return true;
        }

        // The pair whose nearest OPEN portal is closest to `pos` in `dim`,
        // within kNearestRadius; 0 when none.
        uint64_t NearestGun(const glm::dvec3& pos, Game::DimensionId dim) {
            uint64_t best = 0;
            double bestD2 = kNearestRadius * kNearestRadius;
            for (const auto& [gunId, pair] : Game::Portal::ServerRegistry().All()) {
                for (const Game::Portal::Portal* p : {&pair.blue, &pair.orange}) {
                    if (!p->active || p->dimension != dim) continue;
                    const glm::dvec3 d = p->origin - pos;
                    const double d2 = glm::dot(d, d);
                    if (d2 <= bestD2) { bestD2 = d2; best = gunId; }
                }
            }
            return best;
        }

        // A direction word as seen by someone standing in front of the
        // portal looking at it: right = the portal's right axis (its
        // right = up x normal is exactly the viewer's right), up = its up
        // axis (the wall's vertical; a floor/ceiling portal's own up),
        // forward = into the surface (-normal), back = out of it (+normal).
        bool DirectionOf(const std::string& word, const Game::Portal::Portal& p, glm::ivec3& out) {
            const glm::ivec3 right  = glm::ivec3(glm::round(p.right));
            const glm::ivec3 up     = glm::ivec3(glm::round(p.upDir));
            const glm::ivec3 normal = glm::ivec3(glm::round(p.normal));
            if (word == "left")    { out = -right;  return true; }
            if (word == "right")   { out =  right;  return true; }
            if (word == "up")      { out =  up;     return true; }
            if (word == "down")    { out = -up;     return true; }
            if (word == "forward") { out = -normal; return true; }
            if (word == "back")    { out =  normal; return true; }
            return false;
        }

        // `nearest` is nearest the command SOURCE, `@s` the executor.
        void ExecuteMove(const CommandSourceStack& source, const std::vector<std::string>& args, ServerPlayer& sender,
                         ServerConnection& connection) {
            Game::Portal::PortalRegistry& registry = Game::Portal::ServerRegistry();
            // move <target> <color> ...
            size_t i = 1;
            if (i >= args.size()) { connection.SendChatMessage(kMoveUsage, 1); return; }
            uint64_t gunId = 0;
            const std::string target = Lower(args[i]);
            if (target == "gun") {
                if (i + 1 >= args.size()) { connection.SendChatMessage(kMoveUsage, 1); return; }
                errno = 0;
                char* end = nullptr;
                const unsigned long long id = std::strtoull(args[i + 1].c_str(), &end, 10);
                if (errno != 0 || !end || *end != '\0' || id == 0 || args[i + 1][0] == '-') {
                    connection.SendChatMessage("Not a gun id: " + args[i + 1], 1);
                    return;
                }
                gunId = id;
                if (!registry.TryGetPair(gunId)) {
                    connection.SendChatMessage("Gun " + args[i + 1] + " has no open portals", 1);
                    return;
                }
                i += 2;
            } else if (target == "nearest") {
                gunId = NearestGun(source.position, source.dimension);
                if (gunId == 0) {
                    connection.SendChatMessage("No portal-gun portal within 32 blocks of you", 1);
                    return;
                }
                i += 1;
            } else {
                const ServerPlayer* self = source.ExecutorPlayer();
                std::string name = args[i] == "@s" ? (self ? self->getName() : sender.getName()) : args[i];
                const std::vector<uint64_t> guns = Game::Portal::GunsOfPlayer(name);
                if (guns.empty()) {
                    connection.SendChatMessage("No portal-gun portals of " + name, 1);
                    return;
                }
                if (guns.size() > 1) {
                    std::string ids;
                    for (uint64_t g : guns) ids += (ids.empty() ? "" : ", ") + std::to_string(g);
                    connection.SendChatMessage(name + " has " + std::to_string(guns.size()) +
                                               " portal pairs (guns " + ids + ") - use gun <id> or nearest", 1);
                    return;
                }
                gunId = guns.front();
                i += 1;
            }

            if (i >= args.size()) { connection.SendChatMessage(kMoveUsage, 1); return; }
            const std::string color = Lower(args[i++]);
            const bool moveBlue   = color == "blue"   || color == "both";
            const bool moveOrange = color == "orange" || color == "both";
            if (!moveBlue && !moveOrange) { connection.SendChatMessage(kMoveUsage, 1); return; }

            const Game::Portal::PortalPair* pair = registry.TryGetPair(gunId);
            if (!pair) { connection.SendChatMessage("That gun has no open portals", 1); return; }
            if (i >= args.size()) { connection.SendChatMessage(kMoveUsage, 1); return; }

            const std::string how = Lower(args[i++]);
            glm::ivec3 blueDelta(0), orangeDelta(0);
            std::string what;   // "2 blocks left" / "by (1, 0, -2)"
            if (how == "offset") {
                int dx = 0, dy = 0, dz = 0;
                if (i + 3 > args.size() || !ParseInt(args[i], dx) || !ParseInt(args[i + 1], dy) ||
                    !ParseInt(args[i + 2], dz)) {
                    connection.SendChatMessage(kMoveUsage, 1);
                    return;
                }
                i += 3;
                blueDelta = orangeDelta = glm::ivec3(dx, dy, dz);
                what = "by (" + std::to_string(dx) + ", " + std::to_string(dy) + ", " + std::to_string(dz) + ")";
            } else {
                int blocks = 0;
                glm::ivec3 probe;
                if (!DirectionOf(how, pair->blue, probe) || i >= args.size() || !ParseInt(args[i], blocks)) {
                    connection.SendChatMessage(kMoveUsage, 1);
                    return;
                }
                ++i;
                // Each portal along ITS OWN axes: with `both`, "left" is each
                // one's left as seen facing it.
                glm::ivec3 dir;
                if (moveBlue && DirectionOf(how, pair->blue, dir))     blueDelta   = dir * blocks;
                if (moveOrange && DirectionOf(how, pair->orange, dir)) orangeDelta = dir * blocks;
                what = std::to_string(blocks) + (blocks == 1 || blocks == -1 ? " block " : " blocks ") + how;
            }

            bool force = false;
            if (i < args.size()) {
                if (Lower(args[i]) != "force" || i + 1 != args.size()) {
                    connection.SendChatMessage(kMoveUsage, 1);
                    return;
                }
                force = true;
            }

            const std::string owner = pair->owner.empty() ? "gun " + std::to_string(gunId) : pair->owner;
            std::string error;
            if (!registry.MovePortals(gunId, moveBlue, blueDelta, moveOrange, orangeDelta, force, error)) {
                connection.SendChatMessage("Can't move the portal" + std::string(color == "both" ? "s" : "") +
                                           " of " + owner + ": " + error +
                                           " (add 'force' to move anyway)", 1);
                return;
            }
            pair = registry.TryGetPair(gunId);
            auto report = [&](const Game::Portal::Portal& p, const char* name) {
                char at[64];
                std::snprintf(at, sizeof(at), "(%d, %d, %d)",
                              static_cast<int>(std::floor(p.origin.x)), static_cast<int>(std::floor(p.origin.y)),
                              static_cast<int>(std::floor(p.origin.z)));
                source.SendSuccess(connection, std::string("Moved ") + name + " portal of " + owner + " " + what +
                                           " to " + at, true);
            };
            if (pair && moveBlue)   report(pair->blue, "blue");
            if (pair && moveOrange) report(pair->orange, "orange");
            Log::Info("[PortalGunCommand] %s moved %s portal(s) of gun=%llu %s%s", sender.getName().c_str(),
                      color.c_str(), static_cast<unsigned long long>(gunId), what.c_str(), force ? " (forced)" : "");
        }

        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string DescribePortal(const Game::Portal::Portal& p) {
            if (!p.active) return "closed";
            char buf[112];
            std::snprintf(buf, sizeof(buf), "open at %d %d %d (%s)",
                          static_cast<int>(std::floor(p.origin.x)), static_cast<int>(std::floor(p.origin.y)),
                          static_cast<int>(std::floor(p.origin.z)),
                          std::string(Game::DimensionName(p.dimension)).c_str());
            return buf;
        }

        // Close each gun's pair (fizzle burst + PortalRemoveS2C through
        // ClearPair) and report the count.
        void CloseGuns(const CommandSourceStack& source, const std::vector<uint64_t>& guns,
                       const std::string& what, ServerPlayer& sender, ServerConnection& connection) {
            if (guns.empty()) {
                connection.SendChatMessage("No portal-gun portals " + what, 1);
                return;
            }
            for (uint64_t gunId : guns) Game::Portal::ServerRegistry().ClearPair(gunId);
            Log::Info("[PortalGunCommand] %s closed %zu pair(s) %s",
                      sender.getName().c_str(), guns.size(), what.c_str());
            source.SendSuccess(connection, "Closed " + std::to_string(guns.size()) +
                                       " portal-gun pair" + (guns.size() == 1 ? " " : "s ") + what, true);
        }

    } // namespace

    namespace {
        // move <owner|gun <id>|nearest> <blue|orange|both>
        //      (<left|right|up|down|forward|back> <blocks> | offset <dx> <dy> <dz>) [force]
        Game::Cmd::Node MoveSyntax() {
            using namespace Game::Cmd;
            auto forceTail = [] { return Literal("force").Executes(); };
            std::vector<Node> how;
            for (const char* dir : {"left", "right", "up", "down", "forward", "back"}) {
                how.push_back(Literal(dir)
                    .Then(Argument("blocks", Arg::Integer).Suggests({"1", "2", "3"}).Executes()
                        .Then(forceTail())));
            }
            how.push_back(Literal("offset")
                .Then(Argument("dx", Arg::Integer).Suggests({"0", "1", "-1"})
                    .Then(Argument("dy", Arg::Integer).Suggests({"0", "1", "-1"})
                        .Then(Argument("dz", Arg::Integer).Suggests({"0", "1", "-1"}).Executes()
                            .Then(forceTail())))));
            std::vector<Node> colors;
            for (const char* c : {"blue", "orange", "both"}) colors.push_back(Literal(c).Then(how));
            return Literal("move")
                .Then(Literal("nearest").Then(colors))
                .Then(Literal("gun").Then(Argument("id", Arg::Integer).Suggests({"1"}).Then(colors)))
                .Then(Argument("owner", Arg::PlayerName).Then(colors));
        }
    } // namespace

    void PortalGunCommand::Register(CommandDispatcher& dispatcher) {
        using namespace Game::Cmd;
        dispatcher.RegisterCommand("portalgun", PortalGunCommand::Execute,
            Root()
                .Then(Literal("list").Executes())
                .Then(Literal("close")
                    .Then(Literal("all").Executes())
                    .Then(Literal("gun")
                        .Then(Argument("id", Arg::Integer).Suggests({"1"}).Executes()))
                    .Then(Argument("player", Arg::PlayerName).Executes()))
                .Then(Literal("sweep").Executes())
                .Then(MoveSyntax()));
    }

    void PortalGunCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& sessionManager) {
        ServerPlayer& sender = *source.sender;
        Game::Portal::PortalRegistry& registry = Game::Portal::ServerRegistry();
        const std::string sub = args.empty() ? std::string() : Lower(args[0]);

        if (sub == "list" && args.size() == 1) {
            if (registry.All().empty()) {
                connection.SendChatMessage("No portal-gun portals are open", 1);
                return;
            }
            std::vector<uint64_t> guns;
            for (const auto& [gunId, pair] : registry.All()) guns.push_back(gunId);
            std::sort(guns.begin(), guns.end());
            source.SendSuccess(connection, std::to_string(guns.size()) + " portal-gun pair(s):", false);
            for (uint64_t gunId : guns) {
                const Game::Portal::PortalPair* pair = registry.TryGetPair(gunId);
                if (!pair) continue;
                // Owner: who fired it, or (for a pair from before owners were
                // saved) whoever the tracker has found holding the gun; a
                // UUID resolves to the name the server has seen it under.
                std::string owner = registry.PlayerNameFor(pair->owner);
                if (owner.empty() && pair->seen.kind == Game::Portal::GunWhereabouts::Kind::Player) {
                    owner = registry.PlayerNameFor(pair->seen.player);
                }
                // No firer on record (the pair predates owner saving) and no
                // player holds the gun: the first player to pick it up is
                // taken as its owner.
                if (owner.empty()) owner = "not recorded (set when a player next holds the gun)";
                source.SendSuccess(connection, "  gun " + std::to_string(gunId) + " - owner " + owner +
                    " - gun " + Game::Portal::DescribeWhereabouts(gunId), false);
                source.SendSuccess(connection, "    blue " + DescribePortal(pair->blue) +
                    ", orange " + DescribePortal(pair->orange), false);
            }
            return;
        }

        if (sub == "close" && args.size() >= 2) {
            const std::string target = Lower(args[1]);
            if (target == "all" && args.size() == 2) {
                std::vector<uint64_t> guns;
                for (const auto& [gunId, pair] : registry.All()) guns.push_back(gunId);
                CloseGuns(source, guns, "(all)", sender, connection);
                return;
            }
            if (target == "gun" && args.size() == 3) {
                errno = 0;
                char* end = nullptr;
                const unsigned long long id = std::strtoull(args[2].c_str(), &end, 10);
                if (errno != 0 || !end || *end != '\0' || id == 0 || args[2][0] == '-') {
                    connection.SendChatMessage("Not a gun id: " + args[2], 1);
                    return;
                }
                if (!registry.TryGetPair(id)) {
                    connection.SendChatMessage("Gun " + args[2] + " has no open portals", 1);
                    return;
                }
                CloseGuns(source, {static_cast<uint64_t>(id)}, "for gun " + args[2], sender, connection);
                return;
            }
            if (args.size() == 2) {
                // A player's name: online or not (the pair remembers who
                // fired it), @s for yourself.
                std::string name = args[1];
                if (name == "@s") {
                    const ServerPlayer* self = source.ExecutorPlayer();
                    name = self ? self->getName() : sender.getName();
                }
                for (const auto& session : sessionManager.GetAllSessions()) {
                    if (session && session->GetPlayer() && Lower(session->GetPlayer()->getName()) == Lower(name)) {
                        name = session->GetPlayer()->getName();   // their own spelling
                        break;
                    }
                }
                CloseGuns(source, Game::Portal::GunsOfPlayer(name), "of " + name, sender, connection);
                return;
            }
        }

        if (sub == "move") {
            ExecuteMove(source, args, sender, connection);
            return;
        }

        if (sub == "sweep" && args.size() == 1) {
            switch (Game::Portal::StartOrphanSweep(sender.getPlayerId())) {
                case Game::Portal::SweepStart::Started:
                    source.SendSuccess(connection, "Portal gun sweep started - reading every saved chunk and "
                                               "player file for the guns of the open pairs", true);
                    break;
                case Game::Portal::SweepStart::AlreadyRunning:
                    connection.SendChatMessage("A portal gun sweep is already running", 1);
                    break;
                case Game::Portal::SweepStart::NothingToCheck:
                    connection.SendChatMessage("No portal-gun portals are open", 1);
                    break;
                case Game::Portal::SweepStart::NoSaveFolder:
                    connection.SendChatMessage("This world has no save folder to sweep", 1);
                    break;
            }
            return;
        }

        connection.SendChatMessage(kUsage, 1);
    }

} // namespace Server

#endif // ENABLE_PORTAL_GUN
