// File: src/server/commands/GameModeCommand.hpp
// /gamemode command — MC's GameModeCommand.java pattern.
// Usage: /gamemode <survival|creative|adventure|spectator> [player]
//   • Accepts the MC short forms too: s/c/a/sp and the numeric ids 0-3.
//   • With no target, applies to the sender; with a target name, applies
//     to that player (MC's "commands.gamemode.success.other").
//   • Resends PlayerAbilitiesS2C so the client updates HUD/flight/etc.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class GameModeCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
