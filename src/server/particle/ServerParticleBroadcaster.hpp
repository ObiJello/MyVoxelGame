// File: src/server/particle/ServerParticleBroadcaster.hpp
//
// The server half of MC's ServerLevel.sendParticles: a
// ClientboundLevelParticlesPacket (LevelParticlesS2C) to every player of the
// level whose block centre is within 32 blocks of the burst — 512 with
// overrideLimiter (ServerLevel.sendParticles(player, overrideLimiter, …)).
//
// Installed as Game::Particles' ServerParticleSink by IntegratedServer, so
// common code holding a Game::World or a Game::EntityLevel reaches it through
// SendParticles. Threads, as the sound broadcaster: a send from the server
// thread goes out at once (and reports how many players it reached); one from
// a worker is queued and sent by Flush() at the end of the tick.
#pragma once

#include "common/particle/LevelParticles.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace Server {

    class PlayerSessionManager;

    class ServerParticleBroadcaster final : public Game::Particles::ServerParticleSink {
    public:
        explicit ServerParticleBroadcaster(PlayerSessionManager* sessions) : m_sessions(sessions) {}

        int SendParticles(Game::DimensionId dimension, const Game::Particles::ParticleBurst& burst) override;

        // Server thread, once per tick: send what other threads queued.
        void Flush();

    private:
        struct Outgoing {
            Game::DimensionId    dimension = Game::DimensionId::Overworld;
            glm::dvec3           pos{0.0};
            bool                 overrideLimiter = false;
            std::vector<uint8_t> payload;
        };

        int Broadcast(const Outgoing& out) const;

        PlayerSessionManager* m_sessions = nullptr;
        std::mutex            m_mutex;
        std::vector<Outgoing> m_queued;
    };

} // namespace Server
