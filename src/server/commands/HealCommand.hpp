// File: src/server/commands/HealCommand.hpp
//
// `/heal [<target>]` — this engine's own command (vanilla has none): full
// health, a full hunger bar with its saturation, no fire, for the sender
// or whatever the selector names. `/heal @e` heals every mob around you.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class HealCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
