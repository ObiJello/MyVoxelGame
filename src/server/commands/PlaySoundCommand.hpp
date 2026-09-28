// File: src/server/commands/PlaySoundCommand.hpp
//
// MC net.minecraft.server.commands.PlaySoundCommand:
//
//   /playsound <sound> [<source> [<targets> [<pos> [<volume> [<pitch> [<minVolume>]]]]]]
//
// <sound> is any identifier (MC IdentifierArgument — unregistered ids are
// legal and travel inline); "minecraft:" is optional. <source> is a
// SoundSource name (master music record weather block hostile neutral player
// ambient voice ui), default master; <targets> default the calling player;
// <pos> the source's position; volume >= 0 (above 1 the sound carries
// 16 × volume blocks), pitch 0..2, minVolume 0..1.
//
// Per MC playSound: each target in the source's level within
// SoundEvent.getRange(volume) gets a ClientboundSoundPacket at <pos>; one
// farther away hears it only with minVolume > 0, then from two blocks away in
// the sound's direction at minVolume. Feedback is MC's en_us
// (commands.playsound.success.single / .multiple / .failed).
#pragma once
#include "CommandDispatcher.hpp"
#include "common/sound/SoundSource.hpp"

#include <optional>
#include <string>

namespace Server {

    class PlaySoundCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);
        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);

        // Shared with /stopsound.
        // A SoundSource by its MC name ("master", "record", "block" …).
        static std::optional<Game::SoundSource> ParseSource(const std::string& name);
        // MC IdentifierArgument: `[namespace:]path` of [a-z0-9_.-] (path also
        // '/'), with the default namespace dropped ("minecraft:x" → "x"; other
        // namespaces kept). False with `error` set for a malformed id.
        static bool ParseSoundId(const std::string& text, std::string& out, std::string& error);
    };

} // namespace Server
