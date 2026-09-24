// File: src/server/commands/AurelithCommand.hpp
//
// /aurelith — debug and test control of Aurelith's quest (docs/the-hush.md,
// "Reawakening the Heart"; server/level/AurelithCities). Not a vanilla
// command; gated like the other debug commands here (this engine has no
// permission levels yet). Acts on the nearest known city within 640 blocks
// of the sender, in the Hush.
//
//   /aurelith                  the nearest city: Heart, rotation, state
//   /aurelith tp <spot>        go there: heart, podium, soprano, alto, tenor,
//                              bass, vault, gate_soprano|alto|tenor|bass
//   /aurelith keys             the four voice keys, into your inventory
//   /aurelith sing             seat the four keys in the Chord's order (the
//                              real path: the city judges them)
//   /aurelith advance          jump to the next stage (the Unsung rises; the
//                              Unsung falls)
//   /aurelith reset            back to dormant (the lights dimmed, the
//                              sockets emptied, the boss removed)
//   /aurelith heldnote         a Held Note, for testing the reward
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class AurelithCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
