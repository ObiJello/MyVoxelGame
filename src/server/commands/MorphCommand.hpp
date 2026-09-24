// File: src/server/commands/MorphCommand.hpp
//
// `/morph <entity>` — the sender becomes that mob to everyone else: other
// clients draw the mob's model at the player's position with the player's
// body yaw, head yaw, pitch and walking animation, no name tag; the player
// takes the mob's size, eye height and movement speed. `/morph off` (or
// `none` / `player`) restores the player.
//
// Not a vanilla command. Session state only: leaving the world un-morphs.
// The mob type rides PlayerUpdateS2C (every tick, for the others) and
// PlayerAbilitiesS2C (once, for the morphed client's own body).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class MorphCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
