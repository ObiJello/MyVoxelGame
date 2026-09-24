// File: src/client/sound/SoundInstance.hpp
//
// MC client.resources.sounds: SoundInstance, AbstractSoundInstance,
// SimpleSoundInstance, TickableSoundInstance, AbstractTickableSoundInstance
// and EntityBoundSoundInstance — one playing (or queued) sound.
//
// Instances are shared_ptrs: the engine keys its bookkeeping on the instance
// (MC's identity maps) and a caller that wants to stop or test one later —
// the music manager, a looping ambience — keeps its own reference.
//
// TickableSoundInstance is folded into the base as IsTickable / IsStopped /
// Tick, which is how C++ spells MC's `instance instanceof
// TickableSoundInstance` without a cast at every use.
#pragma once

#include "client/sound/WeighedSoundEvents.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundSource.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Client {

    class SoundManager;

    class SoundInstance {
    public:
        enum class Attenuation : uint8_t { None, Linear };

        virtual ~SoundInstance() = default;

        virtual const std::string& GetIdentifier() const = 0;
        // Resolve the event and pick this play's file (MC getOrResolve).
        virtual const WeighedSoundEvents* GetOrResolve(SoundManager& manager) = 0;
        // The picked file; null until resolved, or when the event gave none.
        virtual const Sound* GetSound() const = 0;
        // MC SoundManager.INTENTIONALLY_EMPTY_SOUND: resolved, deliberately silent.
        virtual bool IsIntentionallyEmpty() const = 0;
        virtual const WeighedSoundEvents* GetSoundEvent() const = 0;
        virtual Game::SoundSource GetSource() const = 0;
        virtual bool  IsLooping() const = 0;
        virtual bool  IsRelative() const = 0;
        virtual int   GetDelay() const = 0;
        virtual float GetVolume() = 0;
        virtual float GetPitch() = 0;
        virtual double GetX() const = 0;
        virtual double GetY() const = 0;
        virtual double GetZ() const = 0;
        virtual Attenuation GetAttenuation() const = 0;
        virtual bool CanStartSilent() const { return false; }
        virtual bool CanPlaySound() const { return true; }

        // MC TickableSoundInstance.
        virtual bool IsTickable() const { return false; }
        virtual bool IsStopped() const { return false; }
        virtual void Tick() {}

        // MC SoundInstance.createUnseededRandom → RandomSource.create().
        static int64_t UnseededSeed();
    };

    class AbstractSoundInstance : public SoundInstance {
    public:
        const std::string& GetIdentifier() const override { return m_identifier; }
        const WeighedSoundEvents* GetOrResolve(SoundManager& manager) override;
        const Sound* GetSound() const override { return m_hasSound ? &m_sound : nullptr; }
        bool IsIntentionallyEmpty() const override { return m_intentionallyEmpty; }
        const WeighedSoundEvents* GetSoundEvent() const override { return m_soundEvent; }
        Game::SoundSource GetSource() const override { return m_source; }
        bool  IsLooping() const override { return m_looping; }
        bool  IsRelative() const override { return m_relative; }
        int   GetDelay() const override { return m_delay; }
        // MC: volume * sound.getVolume().sample(random) — the sounds.json
        // numbers are constants, so the sample is the number.
        float GetVolume() override { return m_volume * (m_hasSound ? m_sound.volume : 1.0f); }
        float GetPitch() override { return m_pitch * (m_hasSound ? m_sound.pitch : 1.0f); }
        double GetX() const override { return m_x; }
        double GetY() const override { return m_y; }
        double GetZ() const override { return m_z; }
        Attenuation GetAttenuation() const override { return m_attenuation; }

    protected:
        AbstractSoundInstance(std::string_view identifier, Game::SoundSource source, int64_t seed);

        std::string               m_identifier;
        Game::SoundSource         m_source;
        Game::JavaRandom          m_random;
        const WeighedSoundEvents* m_soundEvent = nullptr;
        Sound                     m_sound;
        bool                      m_hasSound = false;
        bool                      m_intentionallyEmpty = false;
        float                     m_volume = 1.0f;
        float                     m_pitch = 1.0f;
        double                    m_x = 0.0, m_y = 0.0, m_z = 0.0;
        bool                      m_looping = false;
        int                       m_delay = 0;
        Attenuation               m_attenuation = Attenuation::Linear;
        bool                      m_relative = false;
    };

    class SimpleSoundInstance : public AbstractSoundInstance {
    public:
        // MC SimpleSoundInstance(sound, source, volume, pitch, random, x, y, z).
        SimpleSoundInstance(std::string_view event, Game::SoundSource source, float volume, float pitch,
                            int64_t seed, double x, double y, double z);
        // The full constructor (MC's Identifier one).
        SimpleSoundInstance(std::string_view event, Game::SoundSource source, float volume, float pitch,
                            int64_t seed, bool looping, int delay, Attenuation attenuation,
                            double x, double y, double z, bool relative);

        // MC forUI(sound, pitch[, volume = 0.25]): relative, unattenuated.
        static std::shared_ptr<SimpleSoundInstance> ForUI(std::string_view event, float pitch,
                                                          float volume = 0.25f);
        // MC forMusic.
        static std::shared_ptr<SimpleSoundInstance> ForMusic(std::string_view event);
        // MC forJukeboxSong: RECORDS at volume 4 (a 64-block reach).
        static std::shared_ptr<SimpleSoundInstance> ForJukeboxSong(std::string_view event, const glm::dvec3& pos);
        // MC forLocalAmbience / forAmbientAddition / forAmbientMood.
        static std::shared_ptr<SimpleSoundInstance> ForLocalAmbience(std::string_view event, float pitch,
                                                                     float volume);
        static std::shared_ptr<SimpleSoundInstance> ForAmbientAddition(std::string_view event);
        static std::shared_ptr<SimpleSoundInstance> ForAmbientMood(std::string_view event, int64_t seed,
                                                                   double x, double y, double z);
    };

    class AbstractTickableSoundInstance : public AbstractSoundInstance {
    public:
        bool IsTickable() const override { return true; }
        bool IsStopped() const override { return m_stopped; }

    protected:
        using AbstractSoundInstance::AbstractSoundInstance;
        // MC stop(): done, and a looping sound stops looping.
        void Stop() { m_stopped = true; m_looping = false; }

    private:
        bool m_stopped = false;
    };

    // ── Entities, as the sound system sees them ─────────────────────────────
    //
    // EntityBoundSoundInstance follows an entity by id: where it is, whether
    // it is gone, whether it is silent. The client's entity stores (mobs,
    // remote players, the local player) are client code this module does not
    // include, so PlatformMain installs the lookup.
    struct SoundEntityState {
        glm::dvec3 position{0.0};
        bool       removed = false;
        bool       silent = false;
    };
    using SoundEntityResolver = std::function<bool(int32_t entityId, SoundEntityState& out)>;
    void SetSoundEntityResolver(SoundEntityResolver resolver);
    // False when the entity is not known to this client (MC level.getEntity == null).
    bool ResolveSoundEntity(int32_t entityId, SoundEntityState& out);

    // MC EntityBoundSoundInstance: plays at the entity and moves with it,
    // stopping when it is removed; never starts on a silent entity.
    class EntityBoundSoundInstance : public AbstractTickableSoundInstance {
    public:
        EntityBoundSoundInstance(std::string_view event, Game::SoundSource source, float volume, float pitch,
                                 int32_t entityId, int64_t seed);

        bool CanPlaySound() const override;
        void Tick() override;

    private:
        int32_t m_entityId;
    };

} // namespace Client
