// File: src/server/commands/DamageCommand.hpp
//
// MC DamageCommand: `/damage <target> <amount> [<damageType> [at <location> |
// by <entity> [from <cause>]]]` — one hurt through the target's real damage
// path (armour, effects, invulnerability), with the damage type mapped onto the
// engine's damage sources.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class DamageCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
