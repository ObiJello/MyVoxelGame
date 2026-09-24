#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    // MC ForceLoadCommand:
    //   /forceload add <from: x z> [<to: x z>]
    //   /forceload remove <from: x z> [<to: x z>]
    //   /forceload remove all
    //   /forceload query [<pos: x z>]
    // Column positions in blocks (relative ~ allowed); at most 256 chunks per
    // add/remove, in the sender's dimension. Backed by ServerLevel::Keeper().
    class ForceLoadCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
