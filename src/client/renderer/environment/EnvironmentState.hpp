// File: src/client/renderer/environment/EnvironmentState.hpp
//
// Client-side world time + sky/fog/cloud color state, ported from Minecraft's
// post-1.21.9 environment-attribute timeline system (Timelines.java "DAY"
// timeline + AtmosphericFogEnvironment). One instance, updated once per frame;
// every renderer (sky, clouds, chunk fog, portal clear color) reads from it.
//
// Time flow:
//   • Server owns dayTime/gameTime (World::WorldTimeWeatherTick) and syncs
//     every 20 ticks via TimeUpdate (0x19).
//   • OnTimeSync() is called from the network I/O thread → staged in atomics.
//   • TickClient() runs on the client 20-TPS tick (ClientLevel.tickTime
//     mirror): gameTime++ always, dayTime++ only while doDaylightCycle.
//   • UpdateFrame() computes the interpolated frame values from
//     dayTime + partialTick.
#pragma once

#include <atomic>
#include <cstdint>
#include <glm/glm.hpp>
#include "common/world/level/DimensionId.hpp"

namespace Render {

    // Everything the renderers need for one frame, precomputed.
    struct EnvironmentFrame {
        float sunAngleDeg = 0.0f;    // 0° = sun straight up (noon)
        float moonAngleDeg = 180.0f; // sun + 180
        float starAngleDeg = 0.0f;   // = sun angle
        int moonPhase = 0;           // 0..7, 0 = full moon

        glm::vec3 skyColor{0.47f, 0.65f, 1.0f};  // sky disc color
        glm::vec3 fogColor{0.75f, 0.85f, 1.0f};  // final fog/clear color
        glm::vec4 sunriseColor{1, 1, 1, 0};      // sunrise/sunset glow (a = fade)
        glm::vec4 cloudColor{1, 1, 1, 0.8f};     // clouds tint (a = base 0.8)
        float starBrightness = 0.0f;             // 0..0.5
        float skyBrightness = 1.0f;              // terrain dim, 0.26666668..1

        // Fog distances in blocks (MC FogData). rd* pushed to 1e9 when the
        // fog video option is off; skyEnd/cloudEnd always live (horizon fade).
        float fogEnvStart = 0.0f;
        float fogEnvEnd = 1024.0f;
        float fogRdStart = 1e9f;
        float fogRdEnd = 1e9f;
        float fogSkyEnd = 512.0f;
        float fogCloudEnd = 2048.0f;
    };

    class EnvironmentState {
    public:
        static EnvironmentState& Get();

        // Network I/O thread: stage authoritative time from a TimeUpdate.
        void OnTimeSync(uint64_t gameTime, uint64_t dayTime, bool doDaylightCycle);

        // Main thread, 20 TPS client tick (ClientLevel.tickTime mirror).
        void TickClient();

        // Reset to a fresh session (world join) so a stale previous-session
        // time doesn't flash before the first TimeUpdate lands.
        void ResetSession();

        // Main thread, once per frame before any world rendering.
        //   cameraForward: normalized look vector (sunrise fog direction blend)
        //   cameraY:       eye height (dark disc visibility)
        //   renderDistChunks: effective render distance
        //   fogEnabled:    Video Settings fog toggle
        void UpdateFrame(float partialTick, const glm::vec3& cameraForward,
                         float cameraY, int renderDistChunks, bool fogEnabled);

        // The frame renderers read. While a portal view is being drawn, an
        // OVERRIDE — the far dimension's frame — stands in for it.
        const EnvironmentFrame& Frame() const { return m_frameOverride ? *m_frameOverride : m_frame; }
        void SetFrameOverride(const EnvironmentFrame* frame) { m_frameOverride = frame; }
        const EnvironmentFrame* FrameOverride() const { return m_frameOverride; }

        // The frame this moment would have in another dimension (its own
        // fog, sky and ambient rules, this frame's time and camera). Used by
        // the immersive portal renderer for the far side of a portal.
        // `renderDistChunks` composes the fog for that render distance
        // (a view through a portal may draw less far than the main view);
        // 0 keeps the main view's.
        EnvironmentFrame FrameForDimension(Game::DimensionId dimension, int renderDistChunks = 0);

        // Interpolated times for renderers (clouds drift off gameTime).
        double DayTimeF(float partialTick) const;
        double GameTimeF(float partialTick) const;
        int64_t DayTime() const { return m_dayTime; }
        int64_t GameTime() const { return m_gameTime; }
        bool DoDaylightCycle() const { return m_doDaylightCycle; }

        // True when the camera is below the horizon line (MC horizonHeight 63)
        // → SkyRenderer draws the dark disc.
        bool ShouldRenderDarkDisc() const { return m_showDarkDisc; }

        // Skybox fog override (set by SkyRenderer when a cubemap skybox is
        // active). baseFogColor is derived from the skybox's horizon pixels;
        // mode 0 keeps it constant, modes 1/2 multiply it by the night fog
        // curve. Replaces the timeline sky/fog composition so terrain fades
        // into the skybox instead of the vanilla sky color.
        void SetSkyboxOverride(bool active, const glm::vec3& baseFogColor, int mode) {
            m_skyboxActive = active;
            m_skyboxFogBase = baseFogColor;
            m_skyboxMode = mode;
        }

        // MC DimensionSpecialEffects constantAmbientLight (Nether) /
        // forceBrightLightmap (End): neither dimension has a day/night cycle,
        // so the terrain must not dim, the stars must not come out and there
        // is no sunrise glow. Without this a Nether portal — and the whole
        // Nether with it — visibly darkens at midnight for no reason the
        // player can see, because the server keeps ticking overworld time and
        // the sky brightness rides it.
        //
        // Deliberately separate from the skybox override: a player who picks a
        // static skybox in the OVERWORLD should still get night.
        void SetConstantAmbientLight(bool on) { m_constantAmbientLight = on; }

        // Weather strengths, MC Level.getRainLevel / getThunderLevel (0..1,
        // thunder never above rain). The engine has no weather system yet
        // (World::IsRainingAt is a constant false), so nothing sets these
        // and they read 0; an OptiFine sky's `weather` rule is computed
        // from them so it starts working the day weather does.
        void SetWeather(float rainLevel, float thunderLevel) {
            m_rainLevel    = glm::clamp(rainLevel, 0.0f, 1.0f);
            m_thunderLevel = glm::clamp(thunderLevel, 0.0f, m_rainLevel);
        }
        float RainLevel() const    { return m_rainLevel; }
        float ThunderLevel() const { return m_thunderLevel; }

    private:
        EnvironmentState() = default;
        void ApplyPendingSync();

        // Staged sync from the I/O thread.
        std::atomic<bool> m_hasPending{false};
        std::atomic<int64_t> m_pendingGameTime{0};
        std::atomic<int64_t> m_pendingDayTime{6000};
        std::atomic<bool> m_pendingRule{false};

        // Live local time (main thread only).
        int64_t m_gameTime = 0;
        int64_t m_dayTime = 6000;
        bool m_doDaylightCycle = false;
        bool m_showDarkDisc = false;

        // Skybox fog override (main thread only).
        bool m_skyboxActive = false;
        bool m_constantAmbientLight = false;
        float m_rainLevel = 0.0f;
        float m_thunderLevel = 0.0f;
        const EnvironmentFrame* m_frameOverride = nullptr;
        // Last UpdateFrame inputs, so FrameForDimension can recompose.
        float     m_lastPartialTick = 0.0f;
        glm::vec3 m_lastCameraForward{0.0f, 0.0f, 1.0f};
        float     m_lastCameraY = 64.0f;
        int       m_lastRenderDistChunks = 8;
        bool      m_lastFogEnabled = true;
        glm::vec3 m_skyboxFogBase{0.5f};
        int m_skyboxMode = 2;

        EnvironmentFrame m_frame;
    };

} // namespace Render
