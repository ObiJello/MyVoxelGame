// File: src/server/commands/KickCommand.hpp
// /kick command — MC's KickCommand.java pattern.
// Usage: /kick <player> [reason]
//   • Disconnects the named player with an optional reason string.
//   • Reason defaults to "Kicked by an operator" if omitted (MC default).
//   • Cannot kick yourself (matches MC's KickCommand.kickPlayers which
//     short-circuits when the targets list contains the source).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class KickCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
