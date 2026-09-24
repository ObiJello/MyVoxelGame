// File: src/client/sound/SoundManager.hpp
//
// MC net.minecraft.client.sounds.SoundManager: the sound event registry built
// from sounds.json, and the front door to the SoundEngine.
//
// Where the registry comes from (MC's resource stack, per namespace, lowest
// priority first — a later file adds to an event, or with "replace": true
// replaces it):
//   1. assets/sounds.json                      the vanilla file, copied from the
//                                              player's Minecraft install by
//                                              tools/extract_mc_sounds.py
//   2. assets/sound_overlays/<ns>/*.json       the engine's own events (the
//                                              Hush, the mod dimensions),
//                                              mapped onto vanilla sounds with
//                                              "type": "event" references —
//                                              sounds.json format, files in
//                                              name order
//   3. each enabled resource pack's assets/<ns>/sounds.json, bottom to top
// A "file" entry resolves to assets/sounds/<path>.ogg (or a pack's copy);
// one whose file does not exist is dropped with a warning, as MC's
// validateSoundResource.
//
// No assets/sounds.json (never extracted, or a build made without them —
// BUNDLE_MC_SOUNDS=OFF): the vanilla half is read straight out of the
// player's Minecraft install instead (its newest asset index and hashed
// object store), the same files the extraction tool would have copied. No
// install either: one log line and the game runs silent — every Play answers
// NotStarted and the audio device is never opened.
//
// Threads: main thread. Play() from any other thread is queued and started
// on the next Tick (the client's parallel entity tick can emit sounds).
#pragma once

#include "client/sound/SoundEngine.hpp"
#include "client/sound/WeighedSoundEvents.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Client {

    class SoundManager {
    public:
        static SoundManager& Get();

        // Main thread, once, after the resource packs are initialised.
        void Initialize(const std::string& assetsRoot);
        // A resource pack change (MC reload listener): rebuild the registry
        // and restart the engine.
        void ReloadResources();
        // Stop everything and close the device (game exit).
        void Shutdown();
        // The crash path: close the device only.
        void EmergencyShutdown();

        // Any playable sound at all: vanilla files were found (extracted, in
        // a pack, or in the player's Minecraft install).
        bool HasSounds() const { return !m_soundFiles.empty() && m_registry.Size() > 0; }
        const WeighedSoundEvents* GetSoundEvent(const std::string& id) const { return m_registry.Find(id); }
        const SoundRegistry& Registry() const { return m_registry; }
        const WeighedSoundEvents* IntentionallyEmptyEvent() const { return &m_intentionallyEmpty; }

        // MC play / playDelayed / queueTickingSound.
        SoundEngine::PlayResult Play(const std::shared_ptr<SoundInstance>& instance);
        void PlayDelayed(const std::shared_ptr<SoundInstance>& instance, int delay);
        void QueueTickingSound(const std::shared_ptr<SoundInstance>& instance);

        void Stop(const std::shared_ptr<SoundInstance>& instance);
        void Stop(const std::string* identifier, std::optional<Game::SoundSource> source);
        void StopAll();
        bool IsActive(const std::shared_ptr<SoundInstance>& instance) const;

        // MC tick(paused), once per client tick.
        void Tick(bool paused);
        // MC updateSource(camera), once per frame.
        void UpdateSource(const glm::dvec3& position, const glm::dvec3& forward, const glm::dvec3& up);
        glm::dvec3 ListenerPosition() const { return m_listenerPosition; }

        void PauseAllExcept(std::initializer_list<Game::SoundSource> ignored);
        void Resume();
        void UpdateCategoryVolume(Game::SoundSource source, float gain);
        void RefreshCategoryVolume(Game::SoundSource source);

        // ── Options (Music & Sounds) ────────────────────────────────────────
        // Read every sound option from options.txt into the engine.
        void ApplyOptionsFromSettings();
        // A slider moved: the options.txt value is already written; take it
        // and re-apply the volume to what is playing.
        void OnCategoryVolumeChanged(Game::SoundSource source);
        // Device / directional audio changed: restart the library (MC's
        // soundManager.reload() from those options).
        void OnDeviceOptionsChanged();
        std::vector<std::string> GetAvailableSoundDevices() const;
        float GetFinalSoundSourceVolume(Game::SoundSource source) const;

        std::string GetChannelDebugString() const;
        SoundBufferLibrary::Stats GetSoundCacheStats() const;

    private:
        SoundManager() = default;

        void LoadRegistry();
        void LoadSoundsJson(const std::string& path, const std::string& ns);
        // No extracted sounds: fall back to the player's own Minecraft
        // install (its asset index + hashed object store), so a build shipped
        // without Mojang's audio still has sound for anyone who owns the game.
        // Fills m_soundFiles and returns the sounds.json object's path, or "".
        std::string FindMinecraftInstallSounds();
        std::string ResolveSoundFile(const std::string& ns, const std::string& path) const;

        std::string                  m_assetsRoot;
        SoundRegistry                m_registry;
        WeighedSoundEvents           m_intentionallyEmpty;
        std::unique_ptr<SoundEngine> m_engine;
        bool                         m_initialized = false;
        glm::dvec3                   m_listenerPosition{0.0};
        // relative path under sounds/ ("block/stone/break1.ogg") → file.
        std::unordered_map<std::string, std::string> m_soundFiles;

        std::mutex                                  m_crossThreadMutex;
        std::vector<std::shared_ptr<SoundInstance>> m_crossThreadPlays;
    };

    inline SoundManager& GetSoundManager() { return SoundManager::Get(); }

} // namespace Client
