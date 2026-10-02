// File: src/server/commands/PlaceCommand.hpp
//
// MC PlaceCommand, the form this engine can carry out:
//
//   /place feature <feature> [<pos>]
//
// runs a configured feature — a tree, an ore blob, a geode, a fossil, a
// flower patch, an iceberg … — at the position (the source's block by
// default), exactly as worldgen would (Feature.place against the live world,
// server/level/LiveFeatureLevel). Ids are MC's configured-feature keys plus
// the engine dimensions' own (hush:, aether:, twilightforest:).
//
// Not here: MC's inline-SNBT feature definitions (the library builds its
// features in code, it has no configured-feature codec), and `/place
// jigsaw|structure|template` — the library's structure and template
// placement only writes into a generating region (block-entity payloads ride
// the proto chunk), with no live-world path to run them against.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class PlaceCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
