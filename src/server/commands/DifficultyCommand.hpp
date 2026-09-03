#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    // /difficulty [peaceful|easy|normal|hard] — MC DifficultyCommand. No
    // argument reports the world's difficulty. The change reaches every
    // level of the world at once and is saved with it.
    class DifficultyCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
