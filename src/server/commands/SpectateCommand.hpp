// File: src/server/commands/SpectateCommand.hpp
//
// MC SpectateCommand: `/spectate [<target> [<player>]]` — a spectator looks
// through another entity's eyes (Spectator::SetCamera), or gets their own
// back with no target.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class SpectateCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
