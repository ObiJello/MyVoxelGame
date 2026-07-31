// File: src/server/commands/KickCommand.cpp
#include "KickCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include <cctype>
#include <string>

namespace Server {

    void KickCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("kick", KickCommand::Execute);
    }

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

    void KickCommand::Execute(ServerPlayer& sender,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& sessionManager) {
        if (args.empty()) {
            connection.SendChatMessage("Usage: /kick <player> [reason]", 1);
            return;
        }

        const std::string& targetName = args[0];

        // Join args[1..] with spaces for the reason; MC's default is
        // "Kicked by an operator" (translation key
        // multiplayer.disconnect.kicked).
        std::string reason;
        if (args.size() > 1) {
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) reason += " ";
                reason += args[i];
            }
        } else {
            reason = "Kicked by an operator";
        }

        // Resolve target session (PlayerList.getPlayerByName).
        std::shared_ptr<PlayerSession> targetSession;
        for (const auto& session : sessionManager.GetAllSessions()) {
            if (session && session->GetPlayer()) {
                if (CaseInsensitiveEquals(session->GetPlayer()->getName(), targetName)) {
                    targetSession = session;
                    break;
                }
            }
        }
        if (!targetSession || !targetSession->GetPlayer() || !targetSession->GetConnection()) {
            connection.SendChatMessage("Player not found: " + targetName, 1);
            return;
        }

        ServerPlayer*     target     = targetSession->GetPlayer();
        ServerConnection* targetConn = targetSession->GetConnection();

        // MC: KickCommand short-circuits if the command's source IS one of
        // the targets. Keep that — don't let an operator boot themselves.
        if (targetConn == &connection) {
            connection.SendChatMessage("You can't kick yourself", 1);
            return;
        }

        Log::Info("[KickCommand] %s kicked %s (reason: %s)",
                  sender.getName().c_str(),
                  target->getName().c_str(),
                  reason.c_str());

        // Feedback to the issuer first (the disconnect tears the target
        // connection down so its own SendChatMessage would race).
        connection.SendChatMessage("Kicked " + target->getName() + ": " + reason, 1);

        // Send the disconnect packet — the network layer's OnDisconnected
        // callback then runs the usual cleanup (broadcast removal, drop
        // session, etc.) just like a normal disconnect.
        targetConn->SendDisconnect(reason);
    }

} // namespace Server
