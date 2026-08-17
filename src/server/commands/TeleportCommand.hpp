// File: src/server/commands/TeleportCommand.hpp
//
// /tp and /teleport — a port of MC's TeleportCommand.java.
//
// MC declares the whole grammar as one Brigadier tree; this dispatcher hands
// commands a flat token list, so the tree is recovered by shape below. Every
// form vanilla accepts is here:
//
//   /tp <destination>                                    sender to an entity
//   /tp <x> <y> <z>                                      sender to coords
//   /tp <targets> <destination>
//   /tp <targets> <x> <y> <z>
//   /tp <targets> <x> <y> <z> <yaw> <pitch>
//   /tp <targets> <x> <y> <z> facing <fx> <fy> <fz>
//   /tp <targets> <x> <y> <z> facing entity <entity> [feet|eyes]
//
// <targets> and <destination> are full entity selectors (@p @a @r @s @e @n with
// the whole option grammar) — see EntitySelector.hpp. <destination> uses MC's
// `EntityArgument.entity()`, so a selector that could match more than one is a
// hard error rather than a silent pick.
//
// Coordinates take MC's `~` relative form AND its `^left ^up ^forward` local
// form, and both resolve against the COMMAND SOURCE, not against each target.
// That looks surprising for `/tp @e ~ ~ ~` until you follow MC's own path: it resolves against the source, converts to
// a per-victim delta in performTeleport, then adds it back in teleportTo — a
// round trip that cancels out. The observable behaviour is "gather everything
// to me", which is what vanilla does.
//
// NOT ported: cross-dimension teleports (there is one dimension).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class TeleportCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
