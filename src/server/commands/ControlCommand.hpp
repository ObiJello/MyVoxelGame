// File: src/server/commands/ControlCommand.hpp
//
// `/control <player>` — drive that player's game from your own: you see
// through their camera with their HUD, inventory and screens, and your
// keyboard and mouse act as theirs (moving, looking, breaking, placing,
// the inventory, chat, F5). `/control off` ends it. Not a vanilla command;
// the mechanics are in src/server/control/RemoteControlManager.hpp.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class ControlCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
