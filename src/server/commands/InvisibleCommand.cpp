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
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("invisible", InvisibleCommand::Execute,
            Cmd::Root().Executes().Then(Cmd::Literals({"on", "off"})));
    }

    void InvisibleCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& /*sessionManager*/) {
        // The executor (`/execute as Steve run invisible on`).
        ServerPlayer* player = source.ExecutorPlayer();
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
        const bool self = player == source.sender;
        source.SendSuccess(connection, self ? (on ? "You are now invisible to other players"
                                              : "You are visible to other players again")
                                        : player->getName() + (on ? " is now invisible to other players"
                                                                  : " is visible to other players again"), true);
    }

} // namespace Server
