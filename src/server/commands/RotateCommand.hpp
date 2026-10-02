// File: src/server/commands/RotateCommand.hpp
//
// MC RotateCommand: `/rotate <target> <rotation>` |
// `/rotate <target> facing <location>` | `/rotate <target> facing entity
// <entity> [eyes|feet]` — set (or turn by, for `~`) an entity's facing.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class RotateCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
