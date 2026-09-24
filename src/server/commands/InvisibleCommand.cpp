// File: src/server/commands/InvisibleCommand.cpp
#include "InvisibleCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"

#include <cctype>
#include <optional>
#include <string>

namespace Server {

    namespace {
        std::optional<bool> ParseOnOff(const std::string& text) {
            std::string lower;
            for (char c : text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "on"  || lower == "true")  return true;
            if (lower == "off" || lower == "false") return false;
            return std::nullopt;
        }
    } // namespace

    void InvisibleCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("invisible", InvisibleCommand::Execute);
    }

    void InvisibleCommand::Execute(const CommandSourceStack& /*source*/,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& sessionManager) {
        auto session = sessionManager.GetSession(connection.GetPlayerId());
        ServerPlayer* player = session ? session->GetPlayer() : nullptr;
        if (!player) {
            connection.SendChatMessage("Only a player can be invisible", 1);
            return;
        }

        bool on = !player->isInvisible();   // no argument: toggle
        if (!args.empty()) {
            const auto parsed = ParseOnOff(args[0]);
            if (!parsed) {
                connection.SendChatMessage("Usage: /invisible [on|off]", 1);
                return;
            }
            on = *parsed;
        }
        // The flag rides the next PlayerUpdateS2C broadcast (every tick
        // while others are online), so nothing else needs sending here.
        player->setInvisible(on);
        connection.SendChatMessage(on ? "You are now invisible to other players"
                                      : "You are visible to other players again", 1);
    }

} // namespace Server
