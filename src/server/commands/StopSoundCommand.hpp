// File: src/server/commands/StopSoundCommand.hpp
//
// MC net.minecraft.server.commands.StopSoundCommand:
//
//   /stopsound <targets> [<source>|*] [<sound>]
//
// Sends each target MC's ClientboundStopSoundPacket(sound, source) — here the
// stop form of SoundS2C (SoundPackets.hpp's trailing stop flags). No source
// (or "*") stops on every source, no sound stops every sound. "*" must be
// followed by a sound, as in vanilla. Feedback is MC's en_us
// commands.stopsound.success.*.
#pragma once
#include "CommandDispatcher.hpp"

namespace Server {

    class StopSoundCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
