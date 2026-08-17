// File: src/server/commands/SheepEatCommand.hpp
//
// /sheepeat [radius] — force every loaded sheep to start grazing NOW.
//
// DEBUG COMMAND. There is no vanilla equivalent; MC's EatBlockGoal fires on a
// 1-in-1000-per-tick roll, which averages under one graze per sheep per minute
// and makes the animation almost impossible to watch deliberately. Raising the
// tick rate to compensate speeds the animation up by the same factor, so that
// does not help either.
//
// It forces only the DICE. The goal still has to find an edible block, and once
// started it runs the ordinary path — Start broadcasts entity event 10, the
// client plays its own 40-tick animation, the block is consumed at the 4-tick
// mark, and Sheep::OnEatBlock regrows the wool. So what you are watching is the
// real behaviour, not a staged imitation.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class SheepEatCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
