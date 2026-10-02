// File: src/server/commands/PlaySoundCommand.cpp
#include "PlaySoundCommand.hpp"
#include "CommandCoords.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/SoundPackets.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"

#include <cmath>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace Server {

    namespace {

        // Brigadier's number text: "0.0", "2.0" (Java Float.toString).
        std::string FormatBound(double v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", v);
            return buf;
        }

        // Brigadier FloatArgumentType.floatArg(min[, max]).
        bool ParseFloatIn(const std::string& token, double min, std::optional<double> max,
                          double& out, std::string& error) {
            char* end = nullptr;
            const double v = std::strtod(token.c_str(), &end);
            if (token.empty() || !end || *end != '\0' || !std::isfinite(v)) {
                error = "Invalid float '" + token + "'";
                return false;
            }
            if (v < min) {
                error = "Float must not be less than " + FormatBound(min) + ", found " + token;
                return false;
            }
            if (max && v > *max) {
                error = "Float must not be more than " + FormatBound(*max) + ", found " + token;
                return false;
            }
            out = v;
            return true;
        }

        void SendUsage(ServerConnection& connection) {
            connection.SendChatMessage("Unknown or incomplete command, see below for error", 1);
            connection.SendChatMessage(
                "/playsound <sound> [<source>] [<targets>] [<pos>] [<volume>] [<pitch>] [<minVolume>]", 1);
        }

        bool IsIdChar(char c, bool path) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
                   (path && c == '/');
        }

    } // namespace

    std::optional<Game::SoundSource> PlaySoundCommand::ParseSource(const std::string& name) {
        for (int i = 0; i < Game::kSoundSourceCount; ++i) {
            const auto source = static_cast<Game::SoundSource>(i);
            if (Game::SoundSourceName(source) == name) return source;
        }
        return std::nullopt;
    }

    bool PlaySoundCommand::ParseSoundId(const std::string& text, std::string& out, std::string& error) {
        const size_t colon = text.find(':');
        const std::string ns   = colon == std::string::npos ? "minecraft" : text.substr(0, colon);
        const std::string path = colon == std::string::npos ? text : text.substr(colon + 1);
        for (char c : ns) {
            if (!IsIdChar(c, false)) { error = "Non [a-z0-9_.-] character in namespace of location: " + text; return false; }
        }
        for (char c : path) {
            if (!IsIdChar(c, true)) { error = "Non [a-z0-9/._-] character in path of location: " + text; return false; }
        }
        if (path.empty() || ns.empty()) { error = "Invalid ID"; return false; }
        out = std::string(Game::SoundEvents::StripDefaultNamespace(ns + ":" + path));
        return true;
    }

    void PlaySoundCommand::Register(CommandDispatcher& dispatcher) {
        using namespace Game::Cmd;
        // MC's tree: every source literal carries the same optional tail.
        Node tail = Argument("targets", Arg::Players).Executes()
            .Then(Argument("pos", Arg::Vec3).Executes()
                .Then(Argument("volume", Arg::Float).Suggests({"1", "2", "4"}).Executes()
                    .Then(Argument("pitch", Arg::Float).Suggests({"0.5", "1", "2"}).Executes()
                        .Then(Argument("minVolume", Arg::Float).Suggests({"0", "0.5", "1"}).Executes()))));
        std::vector<std::string> sources;
        for (int i = 0; i < Game::kSoundSourceCount; ++i) {
            sources.emplace_back(Game::SoundSourceName(static_cast<Game::SoundSource>(i)));
        }
        dispatcher.RegisterCommand("playsound", PlaySoundCommand::Execute,
            Root().Then(Argument("sound", Arg::Sound).Executes()
                .Then(Literals(sources, true, &tail))));
    }

    void PlaySoundCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& /*sessionManager*/) {
        if (args.empty() || args.size() == 4 || args.size() == 5 || args.size() > 9) {
            // 4-5 tokens = a position cut short (it is three).
            SendUsage(connection);
            return;
        }
        std::string error;
        std::string sound;
        if (!ParseSoundId(args[0], sound, error)) { connection.SendChatMessage(error, 1); return; }

        Game::SoundSource soundSource = Game::SoundSource::Master;
        if (args.size() >= 2) {
            const auto parsed = ParseSource(args[1]);
            if (!parsed) {
                connection.SendChatMessage("Incorrect argument for command", 1);
                connection.SendChatMessage("... " + args[1] + " <--[HERE]", 1);
                return;
            }
            soundSource = *parsed;
        }

        // <targets>, default getCallingPlayerAsCollection: the source's
        // entity when it is a player, else nobody.
        std::vector<SelectedEntity> targets;
        if (args.size() >= 3) {
            if (!ResolveSelector(args[2], SelectorKind::Players, source, targets, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
        } else {
            std::vector<SelectedEntity> self;
            std::string ignored;
            if (ResolveSelector("@s", SelectorKind::Entities, source, self, ignored)) {
                for (SelectedEntity& e : self) {
                    if (e.kind == SelectedEntity::Kind::Player) targets.push_back(std::move(e));
                }
            }
        }

        glm::dvec3 position = source.position;
        if (args.size() >= 6) {
            if (!ParseVec3(args[3], args[4], args[5], source, source.rotation, position, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
        }
        double volume = 1.0, pitch = 1.0, minVolume = 0.0;
        if (args.size() >= 7 && !ParseFloatIn(args[6], 0.0, std::nullopt, volume, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }
        if (args.size() >= 8 && !ParseFloatIn(args[7], 0.0, 2.0, pitch, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }
        if (args.size() >= 9 && !ParseFloatIn(args[8], 0.0, 1.0, minVolume, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        // MC playSound: SoundEvent.createVariableRangeEvent(sound).getRange(volume).
        const double range = Game::SoundEvents::GetRange(static_cast<float>(volume));
        const double maxDistSqr = range * range;
        const int64_t seed = Game::Sound::NextSeed();

        int played = 0;
        std::string lastName;
        for (const SelectedEntity& target : targets) {
            if (!target.session) continue;
            ServerPlayer* player = target.session->GetPlayer();
            ServerConnection* conn = target.session->GetConnection();
            if (!player || !conn) continue;
            // `player.level() != level` — only the source's level hears it.
            if (Game::DimensionFromRaw(player->getDimensionId()) != source.dimension) continue;

            const glm::dvec3 at = player->getPosition();
            const glm::dvec3 delta = position - at;
            const double distSqr = glm::dot(delta, delta);
            glm::dvec3 localPosition = position;
            double localVolume = volume;
            if (distSqr > maxDistSqr) {
                if (minVolume <= 0.0) continue;
                // Out of range but minVolume > 0: two blocks from the player,
                // in the sound's direction, at minVolume.
                const double distance = std::sqrt(distSqr);
                localPosition = at + delta / distance * 2.0;
                localVolume = minVolume;
            }

            Network::SoundS2CPacket packet;
            packet.event  = sound;
            packet.source = soundSource;
            packet.SetPosition(localPosition);
            packet.volume = static_cast<float>(localVolume);
            packet.pitch  = static_cast<float>(pitch);
            packet.seed   = seed;
            conn->SendPacketIn(source.dimension, static_cast<uint8_t>(Network::PacketId::SoundS2C),
                               Network::Serialization::Serialize(packet));
            ++played;
            lastName = player->getName();
        }

        if (played == 0) {
            // commands.playsound.failed
            connection.SendChatMessage("The sound is too far away to be heard", 1);
            return;
        }
        // commands.playsound.success.single / .multiple
        source.SendSuccess(connection, played == 1
            ? "Played sound " + sound + " to " + lastName
            : "Played sound " + sound + " to " + std::to_string(played) + " players", true);
    }

} // namespace Server
