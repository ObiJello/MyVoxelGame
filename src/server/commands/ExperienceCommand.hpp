// File: src/server/commands/ExperienceCommand.hpp
//
// MC ExperienceCommand (and its /xp redirect): `add <targets> <amount>
// [points|levels]`, `set <targets> <amount> [points|levels]`, `query <target>
// points|levels` — over the player's PlayerExperience, which the session's
// SetExperienceS2C dirty-check carries to the client.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class ExperienceCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
