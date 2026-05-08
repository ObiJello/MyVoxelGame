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
            // /tp <x> <y> <z>
            try {
                double x = std::stod(args[0]);
                double y = std::stod(args[1]);
                double z = std::stod(args[2]);

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
            // /tp <player> — teleport to another player
            const std::string& targetName = args[0];

            // Search all sessions for the target player (MC: PlayerList.getPlayerByName)
            ServerPlayer* target = nullptr;
            for (const auto& session : sessionManager.GetAllSessions()) {
                if (session && session->GetPlayer()) {
                    if (CaseInsensitiveEquals(session->GetPlayer()->getName(), targetName)) {
                        target = session->GetPlayer();
                        break;
                    }
                }
            }

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
        } else {
            connection.SendChatMessage("Usage: /tp <x> <y> <z> OR /tp <player>", 1);
        }
    }

} // namespace Server
