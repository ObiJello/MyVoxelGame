// File: src/server/commands/ReplaceAllCommand.hpp
//
// `/replaceall <blocks> <radius> <newblock>` — every block matching any of
// <blocks> within <radius> blocks of the sender becomes <newblock>.
//
// Not a vanilla command (the nearest thing is `/fill … replace <filter>`,
// which wants two corners). The arguments are MC's own: each of <blocks>
// is a BlockPredicate (`oak_log`, `oak_log[axis=y]`, `#minecraft:logs`),
// <newblock> a BlockState (`stone`, `oak_stairs[facing=east]`), `air`
// accepted for both. <blocks> is one predicate or several separated by
// commas, with or without spaces (`grass_block, short_grass 64 air`);
// a comma inside a property list (`oak_log[axis=y,…]`) does not split.
// The radius is a sphere about the source position — the sender's feet,
// or wherever `/execute positioned` put the source. There is no cap on it:
// the sweep walks only the chunks that are loaded, so a radius larger than
// the loaded world costs no more than one that just covers it.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class ReplaceAllCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
