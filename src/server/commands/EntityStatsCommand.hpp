#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    // /entitystats [tnt|all] — a diagnostic dump of the level's mobs: count,
    // position extents, how many sit in chunks that are not entity-ticking
    // (and therefore never advance), and a few sample positions. Added for
    // the mass-detonation work: a handful of primed TNT that never exploded
    // had to be found somewhere, and nothing else could say where.
    class EntityStatsCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
