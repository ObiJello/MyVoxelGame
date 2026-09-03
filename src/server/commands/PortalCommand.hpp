// File: src/server/commands/PortalCommand.hpp
//
// /portal — create, list and remove immersive portals. The subset of the
// Immersive Portals mod's `/portal` command that phase 1 can exercise:
//
//   /portal make       <w> <h> <dim> <x> <y> <z>   one-way, one face
//   /portal make_biway <w> <h> <dim> <x> <y> <z>   two-way (front + reverse)
//   /portal make_full  <w> <h> <dim> <x> <y> <z>   two-way, two faces (the
//                                                  four-record nether cluster)
//   /portal list                                    every portal in your dimension
//   /portal info                                    the nearest portal
//   /portal remove <id>                             that portal and its cluster
//   /portal remove_all                              everything, everywhere
//
// The new portal stands 1.5 blocks in front of you, facing you, bottom edge
// at your feet; <x y z> (with `~`) is where its centre leads to in <dim>
// (overworld | nether | end).
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "CommandDispatcher.hpp"

namespace Server {

    class PortalCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
        // /scale [value] — the player's own size (1 = vanilla), the number a
        // scaled portal multiplies. No value resets it to 1.
        static void ExecuteScale(ServerPlayer& sender,
                                 const std::vector<std::string>& args,
                                 ServerConnection& connection,
                                 PlayerSessionManager& sessionManager);
    };

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
