// File: src/server/commands/ControlCommand.cpp
#include "ControlCommand.hpp"

#include "../IntegratedServer.hpp"
#include "../control/RemoteControlManager.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"

#include <cctype>
#include <string>

namespace Server {

    namespace {
        bool CaseInsensitiveEquals(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        }
    }

    void ControlCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("control", ControlCommand::Execute);
    }

    void ControlCommand::Execute(const CommandSourceStack& source,
                                 const std::vector<std::string>& args,
                                 ServerConnection& connection,
                                 PlayerSessionManager& sessionManager) {
        (void)source;
        if (!g_integratedServer) return;
        RemoteControlManager& control = g_integratedServer->RemoteControl();
        const uint32_t selfId = connection.GetPlayerId();

        if (args.empty()) {
            connection.SendChatMessage("Usage: /control <player> | off");
            return;
        }
        if (CaseInsensitiveEquals(args[0], "off") || CaseInsensitiveEquals(args[0], "stop")) {
            if (!control.TargetOf(selfId) && !control.ControllerOf(selfId)) {
                connection.SendChatMessage("You are not in a control session");
                return;
            }
            control.Stop(selfId, "Control ended");
            return;
        }

        uint32_t targetId = 0;
        std::string targetName;
        for (const auto& session : sessionManager.GetAllSessions()) {
            if (!session || !session->GetPlayer()) continue;
            if (CaseInsensitiveEquals(session->GetPlayer()->getName(), args[0])) {
                targetId   = session->GetPlayerId();
                targetName = session->GetPlayer()->getName();
                break;
            }
        }
        if (!targetId) {
            connection.SendChatMessage("Player not found: " + args[0]);
            return;
        }
        std::string error;
        if (!control.Start(selfId, targetId, error)) {
            connection.SendChatMessage(error);
            return;
        }
        connection.SendChatMessage("Controlling " + targetName + " (/control off to stop)");
    }

} // namespace Server
