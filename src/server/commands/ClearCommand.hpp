// File: src/server/commands/ClearCommand.hpp
//
// MC ClearInventoryCommands: `/clear [<targets> [<item> [<maxCount>]]]` —
// remove (or, with maxCount 0, count) the stacks matching an item predicate
// (ItemPredicateArgument: `*`, an id, `#tag`, `[component tests]`) from the
// players' inventories, the cursor and the 2x2 crafting grid.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class ClearCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
