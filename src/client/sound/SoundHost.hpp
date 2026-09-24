// File: src/client/sound/SoundHost.hpp
//
// The sound half of MC's Minecraft class, as one seam PlatformMain calls:
//
//   Startup / Shutdown     SoundManager construction and destroy (Minecraft
//                          constructor / close).
//   OnFrame                soundManager.updateSource(camera) — every frame,
//                          in the title screen and in the world.
//   Tick                   musicManager.tick(); soundManager.tick(pause) —
//                          every client tick (Minecraft.tick:1975), plus the
//                          level's ambient handlers (LocalPlayer.tick →
//                          AmbientSoundHandler.tick) while in a world, and the
//                          pause edge: pauseAllExcept(MUSIC, UI) when the world
//                          pauses (runTick:1296), resume() when it unpauses
//                          (Gui.setScreen(null)).
//   OnSessionStart / End   the local player for entity-bound sounds, and
//                          updateLevelInEngines' soundManager.stop() on leaving.
#pragma once

#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <string>

namespace Game {
    class ClientPlayer;
    struct IBlockAccess;
}

namespace Client::SoundHost {

    // What the client tick knows that the music and ambience choices read
    // (MC Minecraft.getSituationalMusic / LocalPlayer's ambient handlers).
    struct TickContext {
        bool paused = false;              // MC Minecraft.pause (world paused)
        bool inWorld = false;             // a level and a player exist
        bool levelLoading = false;        // LevelLoadingScreen up: no song starts
        bool creditsScreen = false;       // WinScreen / credits: Musics.CREDITS
        const Game::ClientPlayer*  player = nullptr;
        const Game::IBlockAccess*  blocks = nullptr;   // the player's level
        Game::DimensionId dimension = Game::DimensionId::Overworld;
        glm::dvec3 cameraPosition{0.0};
        bool creative = false;            // abilities.instabuild && abilities.mayfly
        bool underwater = false;          // player.isUnderWater (eye in water)
        bool bossMusic = false;           // End + a boss bar that plays music
    };

    void Startup(const std::string& assetsRoot);
    void Shutdown();
    // Crash path (MC Minecraft.crash → soundManager.emergencyShutdown).
    void EmergencyShutdown();

    void OnFrame(const glm::dvec3& cameraPosition, const glm::vec3& forward, const glm::vec3& up);
    void Tick(const TickContext& context);

    void OnSessionStart(const Game::ClientPlayer* localPlayer);
    void OnSessionEnd();
    // The player's level changed (ChangeDimensionS2C — a portal, /dimension,
    // a respawn elsewhere): MC's respawn path swaps the ClientLevel, whose
    // updateLevelInEngines stops every sound, and — the dimension having
    // changed — MusicManager.stopPlaying rearms the song delay.
    void OnDimensionChanged();

} // namespace Client::SoundHost
