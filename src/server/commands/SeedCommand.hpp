// File: src/server/commands/SeedCommand.hpp
// /seed — prints the world seed as a clickable copy-to-clipboard component,
// mirroring MC's SeedCommand.java.
// Usage: /seed
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SeedCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
