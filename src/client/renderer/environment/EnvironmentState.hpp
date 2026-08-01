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

        const EnvironmentFrame& Frame() const { return m_frame; }

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
        glm::vec3 m_skyboxFogBase{0.5f};
        int m_skyboxMode = 2;

        EnvironmentFrame m_frame;
    };

} // namespace Render
