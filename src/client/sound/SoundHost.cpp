// File: src/client/sound/SoundHost.cpp
#include "client/sound/SoundHost.hpp"

#include "client/sound/AmbientSoundHandlers.hpp"
#include "client/sound/AurelithSounds.hpp"
#include "client/sound/ClientSounds.hpp"
#include "client/sound/MusicManager.hpp"
#include "client/sound/SoundManager.hpp"
#include "common/core/Profiling_Tracy.hpp"

namespace Client::SoundHost {

    namespace {
        bool s_wasPaused = false;
    }

    void Startup(const std::string& assetsRoot) {
        GetSoundManager().Initialize(assetsRoot);
    }

    void Shutdown() {
        MusicManager::Get().StopPlaying();
        AmbientSounds::Reset();
        GetSoundManager().Shutdown();
    }

    void EmergencyShutdown() {
        GetSoundManager().EmergencyShutdown();
    }

    void OnFrame(const glm::dvec3& cameraPosition, const glm::vec3& forward, const glm::vec3& up) {
        GetSoundManager().UpdateSource(cameraPosition, glm::dvec3(forward), glm::dvec3(up));
    }

    void Tick(const TickContext& context) {
        PROFILE_ZONE_N("Sound.Tick");
        SoundManager& manager = GetSoundManager();

        // MC runTick: the moment the world pauses, everything but the music
        // and the UI holds (pauseAllExcept); Gui.setScreen(null) resumes it.
        if (context.paused != s_wasPaused) {
            if (context.paused) manager.PauseAllExcept({Game::SoundSource::Music, Game::SoundSource::Ui});
            else                manager.Resume();
            s_wasPaused = context.paused;
        }

        // MC LocalPlayer.tick → ambientSoundHandlers.forEach(tick). The
        // handlers run on the level's (unpaused) tick.
        if (context.inWorld && !context.paused) {
            AmbientSounds::Tick(context);
            // Aurelith's structure-scoped ambience (the Heart's hum, the
            // gates' thrum, the Spire's wind, the empty choir) — the city is
            // not a biome, so it rides beside the biome handler.
            AurelithSounds::Tick(context);
        }

        // MC Minecraft.tick: musicManager.tick(); soundManager.tick(pause).
        MusicManager::Get().Tick(context);
        manager.Tick(context.paused);
    }

    void OnSessionStart(const Game::ClientPlayer* localPlayer) {
        Sounds::InstallEntityResolver(localPlayer);
    }

    void OnDimensionChanged() {
        // MC ClientPacketListener.handleRespawn: `if (dimensionChanged)
        // musicManager.stopPlaying()` — the song ends and the next one waits
        // MusicFrequency's delay (+100 ticks) — and the new ClientLevel's
        // updateLevelInEngines(level, stopSound = true) stops every sound of
        // the old world, the ambient loops with them (the new LocalPlayer
        // builds its ambient handlers afresh). Same for a seamless portal
        // crossing: MC has none, and the new dimension's ambience and music
        // are what the player should hear from the far side on.
        MusicManager::Get().StopPlaying();
        AmbientSounds::Reset();
        GetSoundManager().StopAll();
    }

    void OnSessionEnd() {
        // MC updateLevelInEngines(level, stopSound = true): every sound of the
        // old world stops, and the ambient loops are gone with the player.
        AmbientSounds::Reset();
        GetSoundManager().StopAll();
        MusicManager::Get().OnWorldChanged();
        SetSoundEntityResolver(nullptr);
        s_wasPaused = false;
    }

} // namespace Client::SoundHost
