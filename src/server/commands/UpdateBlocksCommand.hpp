// File: src/server/commands/UpdateBlocksCommand.hpp
// /updateblocks [radius] — give every block within `radius` of the source
// a block update, as if a neighbour had just changed: MC's
// neighborChanged (the writable reaction — fluids book their spread tick,
// sand falls, torches without a wall pop, redstone re-evaluates) AND the
// six-way updateShape walk (fences re-join, waterlogged blocks book their
// water tick). Engine-only; there is no vanilla equivalent — vanilla has no
// way to poke a stuck block short of placing something beside it.
// Usage: /updateblocks [radius]   (default 8, at most 32)
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class UpdateBlocksCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
