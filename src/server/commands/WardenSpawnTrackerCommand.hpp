// File: src/server/commands/WardenSpawnTrackerCommand.hpp
//
// MC 26.3 WardenSpawnTrackerCommand:
//   /warden_spawn_tracker clear                    — reset the executing
//                                                     player's warning level
//   /warden_spawn_tracker set <warning_level 0-4>  — set it
// (the sculk shrieker's per-player tracker, common/entity/WardenSpawnTracker).
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class WardenSpawnTrackerCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
