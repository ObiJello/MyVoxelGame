// File: src/server/commands/ExecuteCommand.hpp
//
// MC net.minecraft.server.commands.ExecuteCommand.
//
//   /execute (as|at <targets> | positioned (<x> <y> <z> | as <targets> |
//             over <heightmap>) | rotated (<yaw> <pitch> | as <targets>) |
//             facing (<x> <y> <z> | entity <targets> <feet|eyes>) |
//             align <axes> | anchored <feet|eyes> | in <dimension> |
//             summon <entity> | on <relation> |
//             (if|unless) (block <pos> <block> | blocks <start> <end>
//             <destination> (all|masked) | biome <pos> <biome> |
//             dimension <dimension> | entity <targets> | loaded <pos>))...
//            run <command>
//
// Every subcommand is a transformation of the source stack, and
// `as`/`at`/`positioned as`/`rotated as`/`facing entity`/`on`/`if entity`
// FORK it — one stack per matched entity — so what follows runs once per
// fork. `run` hands each stack to the ordinary dispatcher; a trailing
// conditional with no `run` reports "Test passed"/"Test failed" like
// vanilla.
//
// Not here, because the engine has no such system: `store` and `if score`
// (no scoreboard), `if data`/`items`/`slots` (no NBT or slot access),
// `if predicate`/`function` (no data-pack functions or loot predicates),
// `if stopwatch`. Each says so instead of being silently accepted.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class ExecuteCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
