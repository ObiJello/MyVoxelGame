// File: src/server/commands/InvisibleCommand.hpp
//
// `/invisible [on|off]` — hides the sender from every other player: their
// body, name tag, chat bubble and portal ghost stop being drawn on other
// clients until it is turned off again. With no argument it toggles.
//
// Not a vanilla command (the nearest thing is the Invisibility effect,
// which still shows the name tag and any armour). Session state only —
// leaving the world makes the player visible again.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class InvisibleCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
