// File: src/server/commands/SummonCommand.hpp
//
// /summon <type> [count] — MC's SummonCommand.java, reduced to the mobs this
// port implements and always spawning at the sender.
//
// This exists for testing: natural spawning is deliberately slow and
// distance-gated (nothing spawns within 24 blocks of a player), so without a
// direct spawn there is no way to look at a mob on demand.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SummonCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
