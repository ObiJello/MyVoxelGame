// File: src/server/commands/WorldOptionsCommand.hpp
//
// The commands the pause menu's World Options screen (MC WorldOptionsScreen)
// applies its changes through. Vanilla sends most of these as packets
// (ServerboundChangeDifficultyPacket, ServerboundLockDifficultyPacket) or
// calls the IntegratedServer directly from the client thread; this engine's
// host is an ordinary TCP client of its own server, so the same changes ride
// the command channel — which also puts them on the thread that owns the
// server state.
//
//   /defaultgamemode <mode>               MC DefaultGameModeCommand
//   /publish [port]                       MC PublishCommand (LAN scope)
//   /worldoptions allow_commands <on|off>          level.dat allowCommands
//   /worldoptions difficulty_lock                  level.dat DifficultyLocked (one-way)
//   /worldoptions guest_command_access <on|off>    IntegratedServer.guestCommandAccess
//   /worldoptions force_game_mode <on|off>         IntegratedServer.forceGameMode
//   /worldoptions joinable <on|off>                MultiplayerScope LAN / OFF
//   /worldoptions port <1024-65535>                move the listener
//
// Every one of them is host-only (MC: Commands.LEVEL_OWNERS / isSingleplayerOwner).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class WorldOptionsCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
        static void ExecuteDefaultGameMode(const CommandSourceStack& source,
                                           const std::vector<std::string>& args,
                                           ServerConnection& connection,
                                           PlayerSessionManager& sessionManager);
        static void ExecutePublish(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& sessionManager);
    };

} // namespace Server
