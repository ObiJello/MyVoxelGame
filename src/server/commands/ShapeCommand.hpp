#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    // /shape <block> <form> <sizes...> [quirks] [at <x> <y> <z>]
    //
    // A playground builder: geometric arrangements of one block, placed in
    // front of the player (or at explicit coordinates), streamed in over a
    // few ticks so even a hundred-cubed TNT cube never stalls the server.
    //
    //   forms:  cube <s>            solid s x s x s
    //           box <sx> <sy> <sz>  solid box
    //           wall <w> [h]        1-thick wall facing you (h defaults to w)
    //           sphere <r>          ball sitting on the ground
    //           dome <r>            top half of the ball
    //           cylinder <r> [h]    upright (h defaults to r)
    //           pyramid <base>      stepped, Giza-style
    //
    //   quirks: hollow    shell only, one block thick
    //           frame     edges only (cube/box/wall)
    //           checker   3-D checkerboard — a lit corner walks the lattice
    //           spaced=N  one block every N cells in each axis
    //           at x y z  anchor the BASE CENTRE there (~ forms supported)
    //
    // `/shape air ...` carves instead of building.
    class ShapeCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
