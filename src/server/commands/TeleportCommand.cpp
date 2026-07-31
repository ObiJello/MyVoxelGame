// File: src/server/commands/TeleportCommand.cpp
#include "TeleportCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <cctype>
#include <string>

namespace Server {

    void TeleportCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("tp", TeleportCommand::Execute);
        dispatcher.RegisterCommand("teleport", TeleportCommand::Execute);
    }

    // Parse a single coord arg in MC's tilde-relative form. Throws on bad
    // input. Mirrors WorldCoordinate.parseDouble:
    //   "~"     → current
    //   "~5"    → current + 5
    //   "~-3"   → current - 3
    //   "100"   → absolute 100
    //   "100.5" → absolute 100.5
    static double ParseTildeCoord(const std::string& arg, double current) {
        if (arg.empty()) throw std::invalid_argument("empty coord");
        if (arg[0] == '~') {
            if (arg.size() == 1) return current;
            return current + std::stod(arg.substr(1));
        }
        return std::stod(arg);
    }

    // Case-insensitive string comparison
    static bool CaseInsensitiveEquals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    // Resolve a player name to its session (case-insensitive lookup over
    // every active session). Returns nullptr if no match. Mirrors MC's
    // PlayerList.getPlayerByName.
    static std::shared_ptr<PlayerSession> FindSessionByName(
            PlayerSessionManager& sessionManager, const std::string& name) {
        for (const auto& session : sessionManager.GetAllSessions()) {
            if (session && session->GetPlayer()) {
                if (CaseInsensitiveEquals(session->GetPlayer()->getName(), name)) {
                    return session;
                }
            }
        }
        return nullptr;
    }

    void TeleportCommand::Execute(ServerPlayer& sender,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        // Preserve current rotation across teleport — matches MC's
        // teleportToPos(...) which passes entity.getYRot(), entity.getXRot() when no
        // rotation argument is supplied to /tp.
        const float keepYaw   = sender.getYaw();
        const float keepPitch = sender.getPitch();

        if (args.size() == 3) {
            // /tp <x> <y> <z> — each coord may use MC's tilde-relative form
            // (e.g. /tp ~ 200 ~ teleports to current X, Y=200, current Z).
            const glm::dvec3 here = sender.getPosition();
            try {
                double x = ParseTildeCoord(args[0], here.x);
                double y = ParseTildeCoord(args[1], here.y);
                double z = ParseTildeCoord(args[2], here.z);

                // MC: connection.teleport(x, y, z, yRot, xRot) — sets ServerPlayer position,
                // sends ClientboundPlayerPosition with awaiting-teleport id, client snaps + acks.
                connection.Teleport(x, y, z, keepYaw, keepPitch);

                connection.SendChatMessage(
                    "Teleported to " + args[0] + " " + args[1] + " " + args[2], 1);

                Log::Info("[TeleportCommand] Player %s teleported to (%.1f, %.1f, %.1f)",
                         sender.getName().c_str(), x, y, z);
            } catch (const std::exception&) {
                connection.SendChatMessage("Usage: /tp <x> <y> <z>", 1);
            }
        } else if (args.size() == 1) {
            // /tp <player> — sender teleports to that player
            const std::string& targetName = args[0];
            auto targetSession = FindSessionByName(sessionManager, targetName);
            ServerPlayer* target = targetSession ? targetSession->GetPlayer() : nullptr;

            if (target) {
                glm::dvec3 targetPos = target->getPosition();
                connection.Teleport(targetPos.x, targetPos.y, targetPos.z, keepYaw, keepPitch);

                connection.SendChatMessage(
                    "Teleported to " + target->getName(), 1);

                Log::Info("[TeleportCommand] Player %s teleported to player %s",
                         sender.getName().c_str(), target->getName().c_str());
            } else {
                connection.SendChatMessage("Player not found: " + targetName, 1);
            }
        } else if (args.size() == 2) {
            // /tp <sourcePlayer> <targetPlayer> — move source TO target's
            // position. MC's TeleportCommand.teleportToEntity with a player
            // selector resolves to this form.
            const std::string& sourceName = args[0];
            const std::string& targetName = args[1];

            auto sourceSession = FindSessionByName(sessionManager, sourceName);
            auto targetSession = FindSessionByName(sessionManager, targetName);
            ServerPlayer*     source     = sourceSession ? sourceSession->GetPlayer()     : nullptr;
            ServerPlayer*     target     = targetSession ? targetSession->GetPlayer()     : nullptr;
            ServerConnection* sourceConn = sourceSession ? sourceSession->GetConnection() : nullptr;

            if (!source || !sourceConn) {
                connection.SendChatMessage("Player not found: " + sourceName, 1);
                return;
            }
            if (!target) {
                connection.SendChatMessage("Player not found: " + targetName, 1);
                return;
            }

            // Move the SOURCE via its own connection (so it gets the
            // ClientboundPlayerPosition packet and awaits-teleport ack).
            // Preserve the source's own rotation.
            const glm::dvec3 dst = target->getPosition();
            sourceConn->Teleport(dst.x, dst.y, dst.z, source->getYaw(), source->getPitch());

            // Feedback goes to the command sender; if the source was a
            // different player they also get a notice on their screen.
            connection.SendChatMessage(
                "Teleported " + source->getName() + " to " + target->getName(), 1);
            if (sourceConn != &connection) {
                sourceConn->SendChatMessage(
                    "You were teleported to " + target->getName(), 1);
            }

            Log::Info("[TeleportCommand] %s teleported %s to %s",
                     sender.getName().c_str(),
                     source->getName().c_str(),
                     target->getName().c_str());
        } else {
            connection.SendChatMessage(
                "Usage: /tp <x> <y> <z> | /tp <player> | /tp <source> <target>", 1);
        }
    }

} // namespace Server
