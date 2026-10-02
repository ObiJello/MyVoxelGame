// File: src/server/commands/SwingCommand.hpp
//
// MC 26.x SwingCommand: `/swing [<targets> [mainhand|offhand]]` — make living
// entities swing an arm (the swing every watcher sees, the player's own
// first-person arm included).
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class SwingCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
