// File: src/common/particle/LevelParticles.hpp
//
// The server half of MC's particle and level-event networking, as seen from
// common code:
//
//   ServerLevel.sendParticles → ClientboundLevelParticlesPacket to each player
//       of the level within 32 blocks (512 with overrideLimiter) of their
//       block centre (ServerLevel.sendParticles(player, ...)).
//
// (ServerLevel.levelEvent rides the sound sink's LevelEvent —
// Game::Sound::BroadcastLevelEvent, common/sound/LevelSound.hpp — and the
// LevelEventS2C packet; EntityLevel::PlayLevelEvent is its common entry.)
//
// Common code (Game::World, the server's entity bridge) cannot see sessions,
// so the server installs a ServerParticleSink at startup, exactly as it does
// the ServerSoundSink; EntityLevel::SendParticles and its ILevelWrite twin
// reach it from any server level. Absent (a client-only process, a test), a
// server-side send is dropped.
#pragma once

#include "common/particle/ParticleOptions.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Game::Particles {

    // MC ClientboundLevelParticlesPacket.RandomizationType.
    enum class Randomization : uint8_t {
        Default = 0,                  // gaussian spread and speed
        Alternative = 1,              // uniform spread, fixed speed
        AlternativeWithSpeed = 2,     // uniform spread, uniformly scaled speed
        // Engine extension: MC AreaEffectCloud.clientTick's field, run by
        // the receiver — dist.x is the radius, dist.y > 0 the waiting flag
        // (count and speed unused). One packet per cloud per tick stands in
        // for the cloud's DATA_RADIUS / DATA_WAITING / DATA_PARTICLE sync.
        EffectCloud = 3,
    };

    // One ClientboundLevelParticlesPacket's worth of request.
    struct ParticleBurst {
        ParticleOptions options;
        bool          overrideLimiter = false;
        bool          alwaysShow = false;
        glm::dvec3    pos{0.0};
        glm::vec3     dist{0.0f};
        glm::vec3     maxSpeed{0.0f};
        int           count = 0;
        Randomization randomization = Randomization::Default;
    };

    class ServerParticleSink {
    public:
        virtual ~ServerParticleSink() = default;

        // ServerLevel.sendParticles over every player in `dimension`.
        // Returns how many players it was sent to. Callable from any
        // thread (a send from off the server thread is queued).
        virtual int SendParticles(DimensionId dimension, const ParticleBurst& burst) = 0;
    };

    void SetServerSink(ServerParticleSink* sink);
    ServerParticleSink* GetServerSink();

} // namespace Game::Particles
