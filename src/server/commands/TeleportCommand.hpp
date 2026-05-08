// File: src/server/commands/TeleportCommand.hpp
// /tp and /teleport command — MC's TeleportCommand.java pattern.
// Supports: /tp <x> <y> <z>, /tp <player>
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class TeleportCommand {
    public:
        // Register /tp and /teleport aliases with the dispatcher
        static void Register(CommandDispatcher& dispatcher);

        // Command handler
        static void Execute(ServerPlayer& sender,
                           const std::vector<std::string>& args,
                           ServerConnection& connection,
                           PlayerSessionManager& sessionManager);
    };

} // namespace Server
