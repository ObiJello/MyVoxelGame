// File: src/server/commands/KillCommand.hpp
// /kill command — MC's KillCommand.java pattern (self-target only variant
// plus an optional player name, same resolution as /kick).
// Usage: /kill [player]
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class KillCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
