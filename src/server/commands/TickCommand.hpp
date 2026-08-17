// File: src/server/commands/TickCommand.hpp
// /tick — port of MC TickCommand.java.
//
//   /tick query
//   /tick rate <rate>            rate in [1, 10000]
//   /tick freeze | unfreeze
//   /tick step [<time>] | step stop
//   /tick sprint <time> | sprint stop
//
// <time> uses MC's TimeArgument syntax: a number with an optional unit suffix
// (t = ticks, s = seconds x20, d = days x24000; bare number = ticks).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class TickCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
