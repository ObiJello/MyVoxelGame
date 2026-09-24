// File: src/client/sound/SoundEngine.hpp
//
// MC net.minecraft.client.sounds.SoundEngine: which sounds are playing, on
// which channel, at what volume and pitch, and when they end.
//
// Main thread (the client tick and the frame), exactly as MC: play, stop,
// tick and the volume bookkeeping happen here; every OpenAL call is posted to
// the sound executor through the channel handles (ChannelAccess), and decoding
// happens on SoundBufferLibrary's workers. Nothing here waits on either.
//
// MC rules kept verbatim:
//   • pitch clamped to [0.5, 2.0], volume to [0, 1], then × the category's
//     option volume (× master) × the category gain (the music fade);
//   • a sound whose final volume is 0 does not start — except music, and
//     instances that ask to (canStartSilent) — so a muted category costs no
//     channel;
//   • LINEAR attenuation to Sound.getAttenuationDistance(volume)
//     (attenuation_distance, default 16, × the volume above 1); NONE for
//     relative sounds (UI, music);
//   • a finished channel's instance lingers 20 ticks (MIN_SOURCE_LIFETIME)
//     before it is forgotten; a looping sound with a delay re-queues itself;
//   • the pools shed load: a sound that finds no free channel does not play.
#pragma once

#include "client/sound/ChannelAccess.hpp"
#include "client/sound/SoundBufferLibrary.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/audio/Library.hpp"
#include "common/sound/SoundSource.hpp"

#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Client {

    class SoundManager;

    class SoundEngine {
    public:
        enum class PlayResult { Started, StartedSilently, NotStarted };

        explicit SoundEngine(SoundManager& manager);
        ~SoundEngine();

        SoundEngine(const SoundEngine&) = delete;
        SoundEngine& operator=(const SoundEngine&) = delete;

        // MC reload: warn about events with no sounds, then restart the
        // library (a new device, a changed HRTF setting, a pack reload).
        void Reload();
        void Destroy();
        // MC emergencyShutdown: close the device, nothing else (crash path).
        void EmergencyShutdown();
        bool IsLoaded() const { return m_loaded; }

        void Tick(bool paused);

        PlayResult Play(const std::shared_ptr<SoundInstance>& instance);
        void PlayDelayed(const std::shared_ptr<SoundInstance>& instance, int delay);
        void QueueTickingSound(const std::shared_ptr<SoundInstance>& instance);
        void RequestPreload(const std::string& path) { m_preloadQueue.push_back(path); }

        void Stop(const std::shared_ptr<SoundInstance>& instance);
        // MC stop(sound, source): by id and/or category (null = any).
        void Stop(const std::string* identifier, std::optional<Game::SoundSource> source);
        void StopAll();
        bool IsActive(const std::shared_ptr<SoundInstance>& instance) const;

        void PauseAllExcept(std::initializer_list<Game::SoundSource> ignored);
        void Resume();

        // MC updateCategoryVolume: the per-category gain the music fade drives.
        void UpdateCategoryVolume(Game::SoundSource source, float gain);
        // MC refreshCategoryVolume: re-apply after an options change.
        void RefreshCategoryVolume(Game::SoundSource source);

        // MC updateSource(camera): where the listener is, every frame.
        void UpdateSource(const glm::dvec3& position, const glm::dvec3& forward, const glm::dvec3& up);
        Audio::ListenerTransform GetListenerTransform() { return m_library.GetListener().GetTransform(); }

        // ── Options (MC Options' sound half) ────────────────────────────────
        void SetOptionVolume(Game::SoundSource source, float volume);
        // MC Options.getFinalSoundSourceVolume.
        float GetFinalSoundSourceVolume(Game::SoundSource source) const;
        void SetPreferredDevice(const std::string& device) { m_preferredDevice = device; }
        void SetDirectionalAudio(bool on) { m_directionalAudio = on; }

        std::vector<std::string> GetAvailableSoundDevices() const;
        std::string GetChannelDebugString() const { return m_library.GetChannelDebugString(); }
        SoundBufferLibrary::Stats GetSoundCacheStats() const { return m_soundBuffers.GetStats(); }

    private:
        struct Playing {
            std::shared_ptr<SoundInstance> instance;
            std::shared_ptr<ChannelHandle> handle;
        };

        void LoadLibrary();
        bool ShouldChangeDevice();
        void TickInGameSound();
        void TickMusicWhenPaused();
        float CalculatePitch(SoundInstance& instance) const;
        float CalculateVolume(SoundInstance& instance) const;
        float CalculateVolume(float volume, Game::SoundSource source) const;
        void RemoveFromSource(const std::shared_ptr<SoundInstance>& instance);

        SoundManager&            m_manager;
        bool                     m_loaded = false;
        Audio::Library           m_library;
        // Declared after the library so it is destroyed (joined) first.
        SoundEngineExecutor      m_executor;
        ChannelAccess            m_channelAccess;
        SoundBufferLibrary       m_soundBuffers;
        Audio::DeviceTracker     m_deviceTracker;
        Audio::DeviceList        m_lastSeenDevices;
        int                      m_tickCount = 0;

        std::unordered_map<SoundInstance*, Playing>                     m_instanceToChannel;
        std::array<std::vector<std::shared_ptr<SoundInstance>>, Game::kSoundSourceCount> m_instanceBySource;
        std::array<float, Game::kSoundSourceCount>                      m_gainBySource;
        std::vector<std::shared_ptr<SoundInstance>>                     m_tickingSounds;
        std::vector<std::pair<std::shared_ptr<SoundInstance>, int>>     m_queuedSounds;
        std::unordered_map<SoundInstance*, int>                         m_soundDeleteTime;
        std::vector<std::shared_ptr<SoundInstance>>                     m_queuedTickableSounds;
        std::vector<std::string>                                        m_preloadQueue;
        std::unordered_set<std::string>                                 m_onlyWarnOnce;

        std::array<float, Game::kSoundSourceCount> m_optionVolume;
        std::string m_preferredDevice;
        bool        m_directionalAudio = false;
    };

} // namespace Client
