// File: src/client/sound/SoundInstance.cpp
#include "client/sound/SoundInstance.hpp"

#include "client/sound/SoundManager.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>

namespace Client {

    // ── SoundInstance ───────────────────────────────────────────────────────

    int64_t SoundInstance::UnseededSeed() {
        // RandomSource.create(): a fresh seed per call.
        static std::atomic<uint64_t> s_state{
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
        uint64_t z = s_state.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<int64_t>(z ^ (z >> 31));
    }

    // ── AbstractSoundInstance ───────────────────────────────────────────────

    AbstractSoundInstance::AbstractSoundInstance(std::string_view identifier, Game::SoundSource source,
                                                 int64_t seed)
        : m_identifier(SoundRegistry::Normalize(std::string(identifier))),
          m_source(source),
          m_random(seed) {}

    const WeighedSoundEvents* AbstractSoundInstance::GetOrResolve(SoundManager& manager) {
        // MC: the intentionally-empty event resolves to a sound that is
        // deliberately nothing (a sound site that must name an event).
        if (m_identifier == "intentionally_empty") {
            m_intentionallyEmpty = true;
            m_hasSound = false;
            m_soundEvent = manager.IntentionallyEmptyEvent();
            return m_soundEvent;
        }
        m_soundEvent = manager.GetSoundEvent(m_identifier);
        m_hasSound = m_soundEvent && m_soundEvent->GetSound(m_random, manager.Registry(), m_sound);
        return m_soundEvent;
    }

    // ── SimpleSoundInstance ─────────────────────────────────────────────────

    SimpleSoundInstance::SimpleSoundInstance(std::string_view event, Game::SoundSource source, float volume,
                                             float pitch, int64_t seed, double x, double y, double z)
        : SimpleSoundInstance(event, source, volume, pitch, seed, false, 0, Attenuation::Linear, x, y, z,
                              false) {}

    SimpleSoundInstance::SimpleSoundInstance(std::string_view event, Game::SoundSource source, float volume,
                                             float pitch, int64_t seed, bool looping, int delay,
                                             Attenuation attenuation, double x, double y, double z,
                                             bool relative)
        : AbstractSoundInstance(event, source, seed) {
        m_volume = volume;
        m_pitch = pitch;
        m_x = x;
        m_y = y;
        m_z = z;
        m_looping = looping;
        m_delay = delay;
        m_attenuation = attenuation;
        m_relative = relative;
    }

    std::shared_ptr<SimpleSoundInstance> SimpleSoundInstance::ForUI(std::string_view event, float pitch,
                                                                    float volume) {
        return std::make_shared<SimpleSoundInstance>(event, Game::SoundSource::Ui, volume, pitch, UnseededSeed(),
                                                     false, 0, Attenuation::None, 0.0, 0.0, 0.0, true);
    }

    std::shared_ptr<SimpleSoundInstance> SimpleSoundInstance::ForMusic(std::string_view event) {
        return std::make_shared<SimpleSoundInstance>(event, Game::SoundSource::Music, 1.0f, 1.0f, UnseededSeed(),
                                                     false, 0, Attenuation::None, 0.0, 0.0, 0.0, true);
    }

    std::shared_ptr<SimpleSoundInstance> SimpleSoundInstance::ForJukeboxSong(std::string_view event,
                                                                             const glm::dvec3& pos) {
        return std::make_shared<SimpleSoundInstance>(event, Game::SoundSource::Records, 4.0f, 1.0f,
                                                     UnseededSeed(), false, 0, Attenuation::Linear,
                                                     pos.x, pos.y, pos.z, false);
    }

    std::shared_ptr<SimpleSoundInstance> SimpleSoundInstance::ForLocalAmbience(std::string_view event, float pitch,
                                                                               float volume) {
        return std::make_shared<SimpleSoundInstance>(event, Game::SoundSource::Ambient, volume, pitch,
                                                     UnseededSeed(), false, 0, Attenuation::None,
                                                     0.0, 0.0, 0.0, true);
    }

    std::shared_ptr<SimpleSoundInstance> SimpleSoundInstance::ForAmbientAddition(std::string_view event) {
        return ForLocalAmbience(event, 1.0f, 1.0f);
    }

    std::shared_ptr<SimpleSoundInstance> SimpleSoundInstance::ForAmbientMood(std::string_view event, int64_t seed,
                                                                             double x, double y, double z) {
        return std::make_shared<SimpleSoundInstance>(event, Game::SoundSource::Ambient, 1.0f, 1.0f, seed,
                                                     false, 0, Attenuation::Linear, x, y, z, false);
    }

    // ── Entity resolver ─────────────────────────────────────────────────────

    namespace {
        std::mutex& ResolverMutex() {
            static std::mutex m;
            return m;
        }
        SoundEntityResolver& Resolver() {
            static SoundEntityResolver r;
            return r;
        }
    } // namespace

    void SetSoundEntityResolver(SoundEntityResolver resolver) {
        std::lock_guard<std::mutex> lock(ResolverMutex());
        Resolver() = std::move(resolver);
    }

    bool ResolveSoundEntity(int32_t entityId, SoundEntityState& out) {
        std::lock_guard<std::mutex> lock(ResolverMutex());
        return Resolver() ? Resolver()(entityId, out) : false;
    }

    // ── EntityBoundSoundInstance ────────────────────────────────────────────

    EntityBoundSoundInstance::EntityBoundSoundInstance(std::string_view event, Game::SoundSource source,
                                                       float volume, float pitch, int32_t entityId, int64_t seed)
        : AbstractTickableSoundInstance(event, source, seed), m_entityId(entityId) {
        m_volume = volume;
        m_pitch = pitch;
        SoundEntityState state;
        if (ResolveSoundEntity(m_entityId, state)) {
            // MC narrows the entity position through float here.
            m_x = static_cast<float>(state.position.x);
            m_y = static_cast<float>(state.position.y);
            m_z = static_cast<float>(state.position.z);
        }
    }

    bool EntityBoundSoundInstance::CanPlaySound() const {
        SoundEntityState state;
        return ResolveSoundEntity(m_entityId, state) && !state.silent;
    }

    void EntityBoundSoundInstance::Tick() {
        SoundEntityState state;
        if (!ResolveSoundEntity(m_entityId, state) || state.removed) {
            Stop();
            return;
        }
        m_x = static_cast<float>(state.position.x);
        m_y = static_cast<float>(state.position.y);
        m_z = static_cast<float>(state.position.z);
    }

} // namespace Client
