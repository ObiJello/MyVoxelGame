// File: src/server/commands/SetWorldSpawnCommand.hpp
//
// MC SetWorldSpawnCommand:
//
//   /setworldspawn                      the source's block, facing 0 / 0
//   /setworldspawn <pos> [<rotation>]   a spawnable block, and the angle a
//                                       new player faces (yaw pitch)
//
// The world spawn here is the overworld's (the engine's respawn and join
// paths only ever read the overworld's spawn), so the command is refused in
// another dimension with MC's own pre-26 message for exactly that case.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SetWorldSpawnCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
