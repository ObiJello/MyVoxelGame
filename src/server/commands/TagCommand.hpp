// File: src/server/commands/TagCommand.hpp
//
// MC TagCommand: `/tag <targets> add <name> | remove <name> | list` — the
// scoreboard tags every entity carries (Game::EntityTags on mobs, players,
// dropped items and orbs), saved as "Tags" and tested by `@e[tag=…]`.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class TagCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);

        // Re-register the tree with every tag a loaded entity carries as
        // `remove`'s suggestions (MC suggests the targets' tags) and resend
        // the command trees — after a tag was added or removed.
        static void RefreshSuggestions();
    };

} // namespace Server
