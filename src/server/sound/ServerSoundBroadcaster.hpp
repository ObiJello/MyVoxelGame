// File: src/server/sound/ServerSoundBroadcaster.hpp
//
// The server half of MC's Level.playSound: ServerLevel.playSeededSound →
// PlayerList.broadcast(except, x, y, z, range, dimension, packet).
//
// Installed as Game::Sound's ServerSoundSink by IntegratedServer, so common
// code that only holds a Game::World or a Game::EntityLevel reaches it through
// Level::PlaySound. For each sound:
//   • the range is SoundEvent.getRange(volume) — 16 blocks, or 16 × volume
//     above volume 1 (an explosion at 4.0 carries 64 blocks, thunder 10000
//     reaches the whole dimension);
//   • every player in the dimension within that range gets a
//     ClientboundSoundPacket (or ClientboundSoundEntityPacket), EXCEPT the
//     player named by `except` — whose own client already played it while
//     predicting (see common/sound/LevelSound.hpp).
//
// Threads. ServerConnection's world-scoped send is server-thread only, and a
// few sound sources run on worker threads (the parallel falling-block and
// explosion passes). A play from the server thread is sent at once — the
// packet leaves in the same order as the block change it belongs to — and a
// play from anywhere else is queued and sent by Flush() at the end of the
// tick, one tick late at most.
#pragma once

#include "common/sound/LevelSound.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace Server {

    class PlayerSessionManager;

    class ServerSoundBroadcaster final : public Game::Sound::ServerSoundSink {
    public:
        explicit ServerSoundBroadcaster(PlayerSessionManager* sessions) : m_sessions(sessions) {}

        void PlaySound(Game::DimensionId dimension, const Game::SoundExcept& except,
                       const glm::dvec3& pos, std::string_view event,
                       Game::SoundSource source, float volume, float pitch,
                       int64_t seed) override;

        void PlaySoundFromEntity(Game::DimensionId dimension, const Game::SoundExcept& except,
                                 int32_t entityId, const glm::dvec3& pos,
                                 std::string_view event, Game::SoundSource source,
                                 float volume, float pitch, int64_t seed) override;

        // Server thread, once per tick: send what other threads queued.
        void Flush();

    private:
        struct Outgoing {
            Game::DimensionId       dimension = Game::DimensionId::Overworld;
            std::optional<uint32_t> exceptPlayerId;
            glm::dvec3              pos{0.0};
            double                  range = 16.0;
            uint8_t                 packetId = 0;
            std::vector<uint8_t>    payload;
        };

        // The player id `except` names, if it names a player at all (MC:
        // `except instanceof Player` — a mob is never excepted).
        static std::optional<uint32_t> ExceptPlayerId(const Game::SoundExcept& except);

        void Submit(Outgoing&& out);
        void Broadcast(const Outgoing& out) const;

        PlayerSessionManager* m_sessions = nullptr;
        std::mutex            m_mutex;
        std::vector<Outgoing> m_queued;
    };

} // namespace Server
