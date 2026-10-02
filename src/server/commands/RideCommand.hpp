// File: src/server/commands/RideCommand.hpp
//
// MC RideCommand: `/ride <target> mount <vehicle>` | `/ride <target> dismount`
// — players through PlayerRiding (the view joins the vehicle's passengers), mobs
// through Entity::StartRiding, forced as MC's startRiding(vehicle, true, true).
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class RideCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
