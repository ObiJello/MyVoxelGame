// File: src/client/sound/SoundEngine.cpp
#include "client/sound/SoundEngine.hpp"

#include "client/sound/SoundManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/sound/SoundEvents.hpp"

#include <algorithm>
#include <cmath>

namespace Client {

    namespace {
        constexpr float kPitchMin = 0.5f;
        constexpr float kPitchMax = 2.0f;
        constexpr int   kMinSourceLifetime = 20;

        size_t Idx(Game::SoundSource s) { return static_cast<size_t>(s); }

        bool RequiresManualLooping(SoundInstance& i) { return i.GetDelay() > 0; }
        bool ShouldLoopManually(SoundInstance& i) { return i.IsLooping() && RequiresManualLooping(i); }
        bool ShouldLoopAutomatically(SoundInstance& i) { return i.IsLooping() && !RequiresManualLooping(i); }
    } // namespace

    SoundEngine::SoundEngine(SoundManager& manager)
        : m_manager(manager),
          m_channelAccess(m_library, m_executor),
          m_deviceTracker(Audio::DeviceList::Query()) {
        m_gainBySource.fill(1.0f);
        m_optionVolume.fill(1.0f);
        m_lastSeenDevices = m_deviceTracker.CurrentDevices();
    }

    SoundEngine::~SoundEngine() {
        Destroy();
        m_executor.ShutDown();
    }

    // ── Lifecycle ───────────────────────────────────────────────────────────

    void SoundEngine::Reload() {
        m_onlyWarnOnce.clear();
        // MC: every registered SoundEvent with no sounds.json entry is named
        // once in the log — the tell of a missing or partial asset copy.
        int missing = 0;
        for (int i = 0; i < Game::SoundEvents::Count(); ++i) {
            const char* id = Game::SoundEvents::ByRegistryId(i);
            if (!id || std::string_view(id) == "intentionally_empty") continue;
            if (!m_manager.GetSoundEvent(id)) {
                if (missing < 8) Log::Warning("[Sound] Missing sound for event: %s", id);
                m_onlyWarnOnce.insert(id);
                ++missing;
            }
        }
        if (missing > 8) Log::Warning("[Sound] ... and %d more events with no sounds", missing - 8);
        Destroy();
        LoadLibrary();
    }

    void SoundEngine::LoadLibrary() {
        if (m_loaded) return;
        const Audio::DeviceList currentDevices = m_deviceTracker.CurrentDevices();
        if (!m_library.Init(m_preferredDevice, currentDevices, m_directionalAudio)) {
            Log::Error("[Sound] Error starting SoundSystem. Turning off sounds & music");
            return;
        }
        m_executor.Execute([this] { m_library.GetListener().Reset(); });
        m_soundBuffers.Preload(m_preloadQueue);
        m_preloadQueue.clear();
        m_loaded = true;
        Log::Info("[Sound] Sound engine started");
    }

    void SoundEngine::Destroy() {
        if (m_loaded) {
            StopAll();
            m_soundBuffers.Clear();
            m_library.Cleanup();
            m_loaded = false;
        }
    }

    void SoundEngine::EmergencyShutdown() {
        if (m_loaded) m_library.Cleanup();
    }

    // ── Options ─────────────────────────────────────────────────────────────

    void SoundEngine::SetOptionVolume(Game::SoundSource source, float volume) {
        m_optionVolume[Idx(source)] = std::clamp(volume, 0.0f, 1.0f);
    }

    float SoundEngine::GetFinalSoundSourceVolume(Game::SoundSource source) const {
        const float master = m_optionVolume[Idx(Game::SoundSource::Master)];
        return source == Game::SoundSource::Master ? master : m_optionVolume[Idx(source)] * master;
    }

    std::vector<std::string> SoundEngine::GetAvailableSoundDevices() const {
        return m_deviceTracker.CurrentDevices().allDevices;
    }

    void SoundEngine::RefreshCategoryVolume(Game::SoundSource source) {
        if (!m_loaded) return;
        for (auto& [ptr, playing] : m_instanceToChannel) {
            (void)ptr;
            if (source == playing.instance->GetSource() || source == Game::SoundSource::Master) {
                const float volume = CalculateVolume(*playing.instance);
                playing.handle->Execute([volume](Audio::Channel& c) { c.SetVolume(volume); });
            }
        }
    }

    void SoundEngine::UpdateCategoryVolume(Game::SoundSource source, float gain) {
        m_gainBySource[Idx(source)] = std::clamp(gain, 0.0f, 1.0f);
        RefreshCategoryVolume(source);
    }

    float SoundEngine::CalculatePitch(SoundInstance& instance) const {
        return std::clamp(instance.GetPitch(), kPitchMin, kPitchMax);
    }

    float SoundEngine::CalculateVolume(SoundInstance& instance) const {
        return CalculateVolume(instance.GetVolume(), instance.GetSource());
    }

    float SoundEngine::CalculateVolume(float volume, Game::SoundSource source) const {
        return std::clamp(volume, 0.0f, 1.0f)
             * std::clamp(GetFinalSoundSourceVolume(source), 0.0f, 1.0f)
             * m_gainBySource[Idx(source)];
    }

    // ── Stop / pause ────────────────────────────────────────────────────────

    void SoundEngine::Stop(const std::shared_ptr<SoundInstance>& instance) {
        if (!m_loaded || !instance) return;
        auto it = m_instanceToChannel.find(instance.get());
        if (it != m_instanceToChannel.end()) {
            it->second.handle->Execute([](Audio::Channel& c) { c.Stop(); });
        }
    }

    void SoundEngine::StopAll() {
        if (!m_loaded) return;
        // MC: bounce the executor so every queued channel task is dropped,
        // then release every channel from here while it is stopped.
        m_executor.ShutDown();
        m_instanceToChannel.clear();
        m_channelAccess.Clear();
        m_queuedSounds.clear();
        m_tickingSounds.clear();
        for (auto& list : m_instanceBySource) list.clear();
        m_soundDeleteTime.clear();
        m_queuedTickableSounds.clear();
        m_gainBySource.fill(1.0f);
        m_deviceTracker.ResetPending();
        m_executor.StartUp();
    }

    void SoundEngine::Stop(const std::string* identifier, std::optional<Game::SoundSource> source) {
        if (source) {
            // Copy: Stop() does not mutate the list, but a caller's instance
            // may be released while we walk.
            const auto list = m_instanceBySource[Idx(*source)];
            for (const auto& instance : list) {
                if (!identifier || instance->GetIdentifier() == SoundRegistry::Normalize(*identifier)) Stop(instance);
            }
        } else if (!identifier) {
            StopAll();
        } else {
            const std::string id = SoundRegistry::Normalize(*identifier);
            for (auto& [ptr, playing] : m_instanceToChannel) {
                (void)ptr;
                if (playing.instance->GetIdentifier() == id) {
                    playing.handle->Execute([](Audio::Channel& c) { c.Stop(); });
                }
            }
        }
    }

    void SoundEngine::PauseAllExcept(std::initializer_list<Game::SoundSource> ignored) {
        if (!m_loaded) return;
        for (auto& [ptr, playing] : m_instanceToChannel) {
            (void)ptr;
            const Game::SoundSource s = playing.instance->GetSource();
            if (std::find(ignored.begin(), ignored.end(), s) == ignored.end()) {
                playing.handle->Execute([](Audio::Channel& c) { c.Pause(); });
            }
        }
    }

    void SoundEngine::Resume() {
        if (!m_loaded) return;
        m_channelAccess.ExecuteOnChannels([](Audio::Channel& c) { c.Unpause(); });
    }

    bool SoundEngine::IsActive(const std::shared_ptr<SoundInstance>& instance) const {
        if (!m_loaded || !instance) return false;
        const auto del = m_soundDeleteTime.find(instance.get());
        if (del != m_soundDeleteTime.end() && del->second <= m_tickCount) return true;
        return m_instanceToChannel.count(instance.get()) != 0;
    }

    void SoundEngine::RemoveFromSource(const std::shared_ptr<SoundInstance>& instance) {
        auto& list = m_instanceBySource[Idx(instance->GetSource())];
        list.erase(std::remove(list.begin(), list.end(), instance), list.end());
    }

    // ── Device changes ──────────────────────────────────────────────────────

    bool SoundEngine::ShouldChangeDevice() {
        if (m_library.IsCurrentDeviceDisconnected()) {
            Log::Info("[Sound] Audio device was lost!");
            m_deviceTracker.ForceRefresh();
            return true;
        }
        m_deviceTracker.Tick([this](std::function<void()> job) {
            m_executor.Execute(std::move(job));
        });
        bool shouldChange = false;
        const Audio::DeviceList currentDevices = m_deviceTracker.CurrentDevices();
        if (currentDevices != m_lastSeenDevices) {
            const std::string currentName = m_library.CurrentDeviceName();
            if (!currentDevices.Contains(currentName)) {
                Log::Info("[Sound] Current audio device has disappeared!");
                shouldChange = true;
            }
            if (m_preferredDevice.empty()) {
                if (currentName != currentDevices.defaultDevice) {
                    Log::Info("[Sound] System default audio device has changed!");
                    shouldChange = true;
                }
            } else if (currentName != m_preferredDevice && currentDevices.Contains(m_preferredDevice)) {
                Log::Info("[Sound] Preferred audio device has become available!");
                shouldChange = true;
            }
            m_lastSeenDevices = currentDevices;
        }
        return shouldChange;
    }

    // ── Tick ────────────────────────────────────────────────────────────────

    void SoundEngine::Tick(bool paused) {
        PROFILE_ZONE_N("SoundEngine.Tick");
        if (m_loaded && ShouldChangeDevice()) Reload();
        if (!m_loaded) return;
        if (!paused) {
            TickInGameSound();
        } else {
            TickMusicWhenPaused();
        }
        m_channelAccess.ScheduleTick();
    }

    void SoundEngine::TickInGameSound() {
        ++m_tickCount;

        {
            auto queued = std::move(m_queuedTickableSounds);
            m_queuedTickableSounds.clear();
            for (const auto& instance : queued) {
                if (instance->CanPlaySound()) Play(instance);
            }
        }

        // Copy: Stop() only posts, but Play() below may append.
        const auto ticking = m_tickingSounds;
        for (const auto& instance : ticking) {
            if (!instance->CanPlaySound()) Stop(instance);
            instance->Tick();
            if (instance->IsStopped()) {
                Stop(instance);
                continue;
            }
            const float volume = CalculateVolume(*instance);
            const float pitch  = CalculatePitch(*instance);
            const glm::dvec3 position(instance->GetX(), instance->GetY(), instance->GetZ());
            auto it = m_instanceToChannel.find(instance.get());
            if (it != m_instanceToChannel.end()) {
                it->second.handle->Execute([volume, pitch, position](Audio::Channel& c) {
                    c.SetVolume(volume);
                    c.SetPitch(pitch);
                    c.SetSelfPosition(position);
                });
            }
        }

        for (auto it = m_instanceToChannel.begin(); it != m_instanceToChannel.end();) {
            Playing& playing = it->second;
            if (!playing.handle->IsStopped()) { ++it; continue; }
            const auto del = m_soundDeleteTime.find(it->first);
            const int minDeleteTime = del != m_soundDeleteTime.end() ? del->second : 0;
            if (minDeleteTime > m_tickCount) { ++it; continue; }

            std::shared_ptr<SoundInstance> instance = playing.instance;
            if (ShouldLoopManually(*instance)) {
                m_queuedSounds.emplace_back(instance, m_tickCount + instance->GetDelay());
            }
            m_soundDeleteTime.erase(it->first);
            it = m_instanceToChannel.erase(it);
            RemoveFromSource(instance);
            if (instance->IsTickable()) {
                m_tickingSounds.erase(std::remove(m_tickingSounds.begin(), m_tickingSounds.end(), instance),
                                      m_tickingSounds.end());
            }
        }

        for (size_t i = 0; i < m_queuedSounds.size();) {
            if (m_tickCount >= m_queuedSounds[i].second) {
                std::shared_ptr<SoundInstance> instance = std::move(m_queuedSounds[i].first);
                m_queuedSounds[i] = std::move(m_queuedSounds.back());
                m_queuedSounds.pop_back();
                if (instance->IsTickable()) instance->Tick();
                Play(instance);
            } else {
                ++i;
            }
        }
    }

    void SoundEngine::TickMusicWhenPaused() {
        for (auto it = m_instanceToChannel.begin(); it != m_instanceToChannel.end();) {
            Playing& playing = it->second;
            if (playing.instance->GetSource() == Game::SoundSource::Music && playing.handle->IsStopped()) {
                std::shared_ptr<SoundInstance> instance = playing.instance;
                m_soundDeleteTime.erase(it->first);
                it = m_instanceToChannel.erase(it);
                RemoveFromSource(instance);
            } else {
                ++it;
            }
        }
    }

    // ── Play ────────────────────────────────────────────────────────────────

    SoundEngine::PlayResult SoundEngine::Play(const std::shared_ptr<SoundInstance>& instance) {
        if (!m_loaded || !instance) return PlayResult::NotStarted;
        if (!instance->CanPlaySound()) return PlayResult::NotStarted;

        const WeighedSoundEvents* soundEvent = instance->GetOrResolve(m_manager);
        const std::string& eventLocation = instance->GetIdentifier();
        if (!soundEvent) {
            if (m_onlyWarnOnce.insert(eventLocation).second) {
                Log::Warning("[Sound] Unable to play unknown soundEvent: %s", eventLocation.c_str());
            }
            return PlayResult::NotStarted;
        }
        if (instance->IsIntentionallyEmpty()) return PlayResult::NotStarted;
        const Sound* sound = instance->GetSound();
        if (!sound) {
            if (m_onlyWarnOnce.insert(eventLocation).second) {
                Log::Warning("[Sound] Unable to play empty soundEvent: %s", eventLocation.c_str());
            }
            return PlayResult::NotStarted;
        }

        const float instanceVolume = instance->GetVolume();
        const float attenuationDistance = sound->GetAttenuationDistance(instanceVolume);
        const Game::SoundSource soundSource = instance->GetSource();
        const float volume = CalculateVolume(instanceVolume, soundSource);
        bool startedSilently = false;
        if (volume == 0.0f) {
            if (!instance->CanStartSilent() && soundSource != Game::SoundSource::Music) {
                return PlayResult::NotStarted;
            }
            startedSilently = true;
        }

        const glm::dvec3 position(instance->GetX(), instance->GetY(), instance->GetZ());
        const bool isLooping   = ShouldLoopAutomatically(*instance);
        const bool isStreaming = sound->stream;
        std::shared_ptr<ChannelHandle> handle = m_channelAccess.CreateHandle(
            isStreaming ? Audio::Library::Pool::Streaming : Audio::Library::Pool::Static);
        if (!handle) return PlayResult::NotStarted;   // pool full

        m_soundDeleteTime[instance.get()] = m_tickCount + kMinSourceLifetime;
        m_instanceToChannel[instance.get()] = Playing{instance, handle};
        m_instanceBySource[Idx(soundSource)].push_back(instance);

        const float pitch = CalculatePitch(*instance);
        const bool linear = instance->GetAttenuation() == SoundInstance::Attenuation::Linear;
        const bool relative = instance->IsRelative();
        handle->Execute([pitch, volume, linear, attenuationDistance, isLooping, isStreaming, position,
                         relative](Audio::Channel& c) {
            c.SetPitch(pitch);
            c.SetVolume(volume);
            if (linear) c.LinearAttenuation(attenuationDistance);
            else        c.DisableAttenuation();
            c.SetLooping(isLooping && !isStreaming);
            c.SetSelfPosition(position);
            c.SetRelative(relative);
        });

        if (!isStreaming) {
            m_soundBuffers.GetCompleteBuffer(sound->path, [handle](std::shared_ptr<Audio::SoundBuffer> buffer) {
                handle->Execute([buffer](Audio::Channel& c) {
                    if (buffer) {
                        c.AttachStaticBuffer(*buffer);
                        c.Play();
                    } else {
                        // Nothing to play: stop the source so the channel is
                        // released next tick instead of idling forever.
                        c.Stop();
                    }
                });
            });
        } else {
            m_soundBuffers.GetStream(sound->path, isLooping, [handle](std::shared_ptr<Audio::AudioStream> stream) {
                handle->Execute([stream](Audio::Channel& c) {
                    if (stream) {
                        c.AttachBufferStream(stream);
                        c.Play();
                    } else {
                        c.Stop();
                    }
                });
            });
        }

        if (instance->IsTickable()) m_tickingSounds.push_back(instance);
        return startedSilently ? PlayResult::StartedSilently : PlayResult::Started;
    }

    void SoundEngine::QueueTickingSound(const std::shared_ptr<SoundInstance>& instance) {
        m_queuedTickableSounds.push_back(instance);
    }

    void SoundEngine::PlayDelayed(const std::shared_ptr<SoundInstance>& instance, int delay) {
        m_queuedSounds.emplace_back(instance, m_tickCount + delay);
    }

    void SoundEngine::UpdateSource(const glm::dvec3& position, const glm::dvec3& forward, const glm::dvec3& up) {
        if (!m_loaded) return;
        Audio::ListenerTransform transform{position, forward, up};
        m_executor.Execute([this, transform] { m_library.GetListener().SetTransform(transform); });
    }

} // namespace Client
