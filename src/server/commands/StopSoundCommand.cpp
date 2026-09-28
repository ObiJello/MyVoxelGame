// File: src/server/commands/StopSoundCommand.cpp
#include "StopSoundCommand.hpp"
#include "PlaySoundCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/SoundPackets.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Server {

    void StopSoundCommand::Register(CommandDispatcher& dispatcher) {
        using namespace Game::Cmd;
        Node sound = Argument("sound", Arg::Sound).Executes();
        Node targets = Argument("targets", Arg::Players).Executes();
        // "*": any source, and then a sound is required (not executable).
        targets.Then(Literal("*").Then(sound));
        for (int i = 0; i < Game::kSoundSourceCount; ++i) {
            targets.Then(Literal(std::string(Game::SoundSourceName(static_cast<Game::SoundSource>(i))))
                             .Executes()
                             .Then(sound));
        }
        dispatcher.RegisterCommand("stopsound", StopSoundCommand::Execute, Root().Then(std::move(targets)));
    }

    void StopSoundCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& /*sessionManager*/) {
        if (args.empty() || args.size() > 3 || (args.size() == 2 && args[1] == "*")) {
            connection.SendChatMessage("Unknown or incomplete command, see below for error", 1);
            connection.SendChatMessage("/stopsound <targets> [<source>|*] [<sound>]", 1);
            return;
        }
        std::string error;
        std::vector<SelectedEntity> targets;
        if (!ResolveSelector(args[0], SelectorKind::Players, source, targets, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        std::optional<Game::SoundSource> soundSource;
        if (args.size() >= 2 && args[1] != "*") {
            soundSource = PlaySoundCommand::ParseSource(args[1]);
            if (!soundSource) {
                connection.SendChatMessage("Incorrect argument for command", 1);
                connection.SendChatMessage("... " + args[1] + " <--[HERE]", 1);
                return;
            }
        }
        std::string sound;   // empty = every sound
        if (args.size() == 3 && !PlaySoundCommand::ParseSoundId(args[2], sound, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        // ClientboundStopSoundPacket(sound, source) to every target. Scoped
        // to the target's own level (the client applies a stop whatever
        // level the sound was started in).
        const std::vector<uint8_t> payload = Network::Serialization::Serialize(
            Network::SoundS2CPacket::Stop(sound, soundSource.has_value(),
                                          soundSource.value_or(Game::SoundSource::Master)));
        for (const SelectedEntity& target : targets) {
            if (!target.session) continue;
            ServerPlayer* player = target.session->GetPlayer();
            ServerConnection* conn = target.session->GetConnection();
            if (!player || !conn) continue;
            conn->SendPacketIn(Game::DimensionFromRaw(player->getDimensionId()),
                               static_cast<uint8_t>(Network::PacketId::SoundS2C), payload);
        }

        if (soundSource) {
            const std::string name(Game::SoundSourceName(*soundSource));
            connection.SendChatMessage(sound.empty()
                ? "Stopped all '" + name + "' sounds"                                // .source.any
                : "Stopped sound '" + sound + "' on source '" + name + "'", 1);     // .source.sound
        } else {
            connection.SendChatMessage(sound.empty()
                ? std::string("Stopped all sounds")                                  // .sourceless.any
                : "Stopped sound '" + sound + "'", 1);                               // .sourceless.sound
        }
    }

} // namespace Server
