// File: src/server/commands/SummonCommand.hpp
//
// /summon <entity> [count] [<pos>] [<nbt>] [fuse=n] [delay=n] — MC's
// SummonCommand.java plus the engine's count and TNT options. <nbt> is MC's
// CompoundTagArgument (SNBT, SnbtParser.hpp), loaded onto each new entity by
// the same reader a saved entity goes through (EntityNbt ApplyMobNbt); with
// one, finalizeSpawn is skipped as in MC, so `{variant:"ashen"}` stands.
//
// This exists for testing: natural spawning is deliberately slow and
// distance-gated (nothing spawns within 24 blocks of a player), so without a
// direct spawn there is no way to look at a mob on demand.
#pragma once

#include "CommandDispatcher.hpp"
#include "common/entity/EntityType.hpp"

namespace Server {

    class SummonCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);

        // MC ResourceArgument for an entity type: "[minecraft:]zombie".
        // Shared with `/execute summon`.
        static bool ParseEntityType(const std::string& text, Game::EntityTypeId& out);
    };

} // namespace Server
