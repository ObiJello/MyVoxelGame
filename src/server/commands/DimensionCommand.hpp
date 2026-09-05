// File: src/server/commands/DimensionCommand.hpp
// /dimension (alias /dim) <overworld|nether|end> — travel to another
// dimension the way its portal would take you from where you stand:
//   • end        the End-portal rule: the obsidian platform at
//                END_SPAWN_POINT (100, 50, 0), built on arrival, facing west
//   • nether     the nether-portal rule from your position: x and z scaled
//                by 1/8, y kept, the nearest portal within 128 blocks reused;
//                otherwise the spot a new portal would take, with NO portal
//                built — you land where the crossing would have put you
//   • overworld  from the Nether the nether-portal rule (x8, same search);
//                from the End the End-portal rule (the world spawn)
// Names accept MC's forms too: the_nether, the_end, minecraft:overworld.
// Not a vanilla command (vanilla needs /execute in <dim> run tp, which
// skips the portal logic); it reuses PortalTravel so the outcome is what a
// portal would have done.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class DimensionCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
