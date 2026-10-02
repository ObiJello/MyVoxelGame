// File: src/server/commands/SpreadPlayersCommand.hpp
//
// MC SpreadPlayersCommand:
//
//   /spreadplayers <center> <spreadDistance> <maxRange> [under <maxHeight>] <respectTeams> <targets>
//
// Scatters the targets over the square of half-side <maxRange> around the
// column <center> (x z), at least <spreadDistance> apart, each on the top
// standable block below <maxHeight> (the build top by default) — vanilla's
// relaxation loop verbatim (10 000 rounds, then "too many entities").
// There are no scoreboard teams in this engine, so <respectTeams> true
// treats every target as team-less: one position, all of them on it — which
// is what vanilla does with team-less entities too.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SpreadPlayersCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
