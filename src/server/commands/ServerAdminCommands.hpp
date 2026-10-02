// File: src/server/commands/ServerAdminCommands.hpp
//
// MC's dedicated-server-only commands this engine has the machinery for
// (Commands.java registers them under `commandSelection.includeDedicated`,
// so an integrated — singleplayer / LAN-hosted — world does not have them,
// and neither does this engine's: they are registered only on a server
// without a singleplayer owner, the --headless-server):
//
//   /save-all [flush]   MC SaveAllCommand  — IntegratedServer::SaveEverything
//   /save-off           MC SaveOffCommand  — stops the autosave (SetAutoSave)
//   /save-on            MC SaveOnCommand
//   /stop               MC StopCommand     — "Stopping the server", then the
//                                            headless loop shuts it down
//                                            (IntegratedServer::RequestHalt)
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class ServerAdminCommands {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void ExecuteSaveAll(const CommandSourceStack& source, const std::vector<std::string>& args,
                                   ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteSaveOff(const CommandSourceStack& source, const std::vector<std::string>& args,
                                   ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteSaveOn(const CommandSourceStack& source, const std::vector<std::string>& args,
                                  ServerConnection& connection, PlayerSessionManager& sessionManager);
        static void ExecuteStop(const CommandSourceStack& source, const std::vector<std::string>& args,
                                ServerConnection& connection, PlayerSessionManager& sessionManager);
    };

} // namespace Server
