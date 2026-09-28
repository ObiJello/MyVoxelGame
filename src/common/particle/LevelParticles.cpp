// File: src/common/particle/LevelParticles.cpp
#include "common/particle/LevelParticles.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <atomic>

namespace Game::Particles {

    namespace {
        std::atomic<ServerParticleSink*> g_sink{nullptr};
    }

    void SetServerSink(ServerParticleSink* sink) { g_sink.store(sink, std::memory_order_release); }
    ServerParticleSink* GetServerSink() { return g_sink.load(std::memory_order_acquire); }

    namespace {

        int Send(DimensionId dimension, const ParticleOptions& options, bool overrideLimiter, bool alwaysShow,
                 double x, double y, double z, int count, double xDist, double yDist, double zDist,
                 double speed) {
            ServerParticleSink* sink = GetServerSink();
            if (!sink) return 0;
            ParticleBurst burst;
            burst.options = options;
            burst.overrideLimiter = overrideLimiter;
            burst.alwaysShow = alwaysShow;
            burst.pos = glm::dvec3(x, y, z);
            // MC: (float)xDist ... and the one speed for all three axes.
            burst.dist = glm::vec3(static_cast<float>(xDist), static_cast<float>(yDist), static_cast<float>(zDist));
            burst.maxSpeed = glm::vec3(static_cast<float>(speed));
            burst.count = count;
            burst.randomization = Randomization::Default;
            return sink->SendParticles(dimension, burst);
        }

    } // namespace

} // namespace Game::Particles

namespace Game {

    int EntityLevel::SendParticles(const ParticleOptions& options, bool overrideLimiter, bool alwaysShow,
                                   double x, double y, double z, int count,
                                   double xDist, double yDist, double zDist, double speed) {
        if (IsClientSide()) return 0;
        return Particles::Send(Dimension(), options, overrideLimiter, alwaysShow, x, y, z, count,
                               xDist, yDist, zDist, speed);
    }

    void EntityLevel::PlayLevelEvent(const SoundExcept& except, int type, const glm::ivec3& pos, int data) {
        if (IsClientSide()) return;
        Sound::BroadcastLevelEvent(Dimension(), except, type, pos, data);
    }

    int ILevelWrite::SendParticles(const ParticleOptions& options, bool overrideLimiter, bool alwaysShow,
                                   double x, double y, double z, int count,
                                   double xDist, double yDist, double zDist, double speed) {
        if (IsClientSide()) return 0;
        return Particles::Send(GetDimension(), options, overrideLimiter, alwaysShow, x, y, z, count,
                               xDist, yDist, zDist, speed);
    }

    void ILevelWrite::PlayLevelEvent(const SoundExcept& except, int type, const glm::ivec3& pos, int data) {
        if (IsClientSide()) return;
        Sound::BroadcastLevelEvent(GetDimension(), except, type, pos, data);
    }

} // namespace Game
