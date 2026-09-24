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
#include <chrono>
#include <cstdint>
#include <glm/glm.hpp>
#include "common/world/level/DimensionId.hpp"
#include "common/world/biome/Biomes.hpp"

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
        // The pre-light-engine terrain dim (MC SKY_LIGHT_LEVEL / 15,
        // 0.26666668..1). Still what OBEY_LIGHT=0 draws with and what the sky
        // and HUD passes read; the world itself is lit through the lightmap.
        float skyBrightness = 1.0f;

        // MC's lightmap inputs from the environment attributes (Lightmap.hpp):
        float     skyLightFactor = 1.0f;         // SKY_LIGHT_FACTOR (day 1 .. night 0.24)
        glm::vec3 skyLightColor{1.0f};           // SKY_LIGHT_COLOR (white .. #7a7aff)
        glm::vec3 ambientLightColor{10.0f / 255.0f};   // AMBIENT_LIGHT_COLOR (dimension)
        glm::vec3 blockLightTint{1.0f, 216.0f / 255.0f, 140.0f / 255.0f};   // BLOCK_LIGHT_TINT #ffd88c

        // Fog distances in blocks (MC FogData). rd* pushed to 1e9 when the
        // fog video option is off; skyEnd/cloudEnd always live (horizon fade).
        float fogEnvStart = 0.0f;
        float fogEnvEnd = 1024.0f;
        float fogRdStart = 1e9f;
        float fogRdEnd = 1e9f;
        float fogSkyEnd = 512.0f;
        float fogCloudEnd = 2048.0f;

        // Sun and moon opacity. 1 everywhere but the Aether, whose sky fades
        // them in and out around dusk and dawn (AetherSkyRenderEffects
        // .drawCelestialBodies).
        float sunAlpha = 1.0f;
        float moonAlpha = 1.0f;
        // Bottom of the cloud layer, world Y — MC CLOUD_HEIGHT (overworld
        // 192.33). The Aether's clouds float under its islands at 9.83
        // (AetherSkyRenderEffects cloudLevel 9.5 + renderClouds' 0.33).
        float cloudBottomY = 192.33f;

        // The Hush's aurora ribbons, 0..1 (AuroraRenderer draws them in the
        // Hush's sky pass). Composed by HushAtmosphere; 0 everywhere else.
        float auroraStrength = 0.0f;
    };

    // Which dimension's sky-and-fog rules compose the frame, beyond the
    // flags below (skybox override, constant ambient, fixed night): the two
    // ported mods' dimensions bring their own composition.
    //   Vanilla         MC's DAY timeline + AtmosphericFogEnvironment
    //   TwilightForest  TF 4.9 on MC 26.1: the dimension's attributes (no
    //                   DAY timeline — "#minecraft:universal"), the biomes'
    //                   fog/sky colours blended around the camera, and
    //                   FogHandler's dusk dimming (TwilightForestRenderInfo)
    //   Aether          The Aether 1.5.10 on MC 1.21.1: AetherSkyRenderEffects
    //                   (sky, sunrise, clouds, sun/moon fade) over the 1.21.1
    //                   FogRenderer with DimensionClientHooks' fog changes
    enum class Atmosphere : uint8_t { Vanilla, TwilightForest, Aether };

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

        // MC DimensionType.fixedTime on a dimension that HAS a sky (the
        // Hush; vanilla only fixes the clock where the sky is NONE or END).
        // The server pins the Hush's clock at midnight (DimensionFixedTime),
        // but the client's one clock is the Overworld's — TimeUpdateS2C
        // carries a single dayTime for the session — so the frame is pinned
        // here instead: the terrain sits at the Overworld's night floor, the
        // stars are fully out and there is no sunrise glow, whatever the
        // Overworld's time is. Set together with the constant ambient light
        // (SkyRenderer::ApplyDimensionSky); the fixed night refines "no
        // cycle" from noon to midnight.
        void SetFixedNight(bool on) { m_fixedNight = on; }

        // The dimension whose AMBIENT_LIGHT_COLOR and sky light colour the
        // lightmap uses (set with the rest by SkyRenderer::ApplyDimensionSky).
        void SetLightDimension(Game::DimensionId dimension) { m_lightDimension = dimension; }

        // The mod dimensions' own sky and fog (see Atmosphere). Set by
        // SkyRenderer::ApplyDimensionSky with the other dimension rules.
        void SetAtmosphere(Atmosphere atmosphere) { m_atmosphere = atmosphere; }
        Atmosphere GetAtmosphere() const { return m_atmosphere; }

        // Where the camera is, for the per-biome attributes the Twilight
        // Forest blends around it (MC EnvironmentAttributeProbe samples the
        // biomes around Camera.position). Set every frame BEFORE UpdateFrame.
        void SetCameraPosition(const glm::dvec3& position) {
            m_cameraPos = position;
            m_hasCameraPos = true;
        }

        // The Aether's AetherTimeAttachment.isEternalDay: true (the clock
        // held at noon) until the Sun Spirit is defeated — which this port
        // has no boss for yet, so it stays true. Cleared, the Aether's sky
        // runs on its own 72,000-tick day (three Overworld days, the mod's
        // default AetherTimeAttachment.getTicksPerDay) off the client clock.
        void SetAetherEternalDay(bool on) { m_aetherEternalDay = on; }

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

        // MC ClientLevel.skyFlashTime — set to 2 by a flashing LightningBolt
        // (ClientLevelBridge::SetSkyFlashTime, which also honours "Hide
        // Lightning Flashes"), counted down in TickClient. While it is
        // positive UpdateFrame applies ClientLevel's two environment layers:
        // SKY_COLOR lerped 0.22 toward (0.8, 0.8, 1.0) — the fog follows,
        // since it is blended toward the sky colour — and SKY_LIGHT_FACTOR
        // forced to 1 (skyBrightness, the terrain's day/night dim).
        void SetSkyFlashTime(int ticks) { m_skyFlashTime = ticks; }
        int  SkyFlashTime() const { return m_skyFlashTime; }

        // MC FogRenderer's fluid fog (WaterFogEnvironment / LavaFogEnvironment).
        // `fluid` is Camera.getFluidInCamera: 0 none, 1 water, 2 lava.
        // `biomeAtEye` supplies the biome's WATER_FOG_COLOR; `spectator`
        // picks lava's see-through variant. Set every frame BEFORE
        // UpdateFrame; TickClient runs LocalPlayer's waterVisionTime off it.
        enum CameraFluid : int { kFluidNone = 0, kFluidWater = 1, kFluidLava = 2 };
        // `resonantWater`: the eye's water is Aurelith's resonant water (the
        // river Vesper, BlockID::ResonantWater) — the water fog turns its
        // luminous violet instead of the biome's colour (docs/fluids.md).
        void SetCameraFluid(int fluid, Game::BiomeId biomeAtEye, bool spectator,
                            bool resonantWater = false);
        int  CameraFluid() const { return m_cameraFluid; }
        // MC LocalPlayer.getWaterVision: 0..1, the eyes adjusting to water
        // over 600 ticks; water fog distance scales by max(0.25, this).
        float WaterVision() const;

    private:
        EnvironmentState() = default;
        void ApplyPendingSync();

        // The mod dimensions' composition (see Atmosphere), run by
        // UpdateFrame in place of the vanilla sky/fog colours, before the
        // fluid fog.
        void ComposeTwilightForest(int renderDistChunks, bool fogEnabled);
        void ComposeAether(float partialTick, const glm::vec3& cameraForward,
                           int renderDistChunks, bool fogEnabled);
        // TF FogHandler.colorFog: the last word on the fog colour in the
        // Twilight Forest, fluid fog included (ViewportEvent.ComputeFogColor).
        void ApplyTwilightFogHandler();

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
        bool m_fixedNight = false;
        Game::DimensionId m_lightDimension = Game::DimensionId::Overworld;
        // Set while FrameForDimension recomposes another dimension's frame.
        bool m_recomposing = false;
        // Fills the frame's lightmap inputs (skyLightFactor & co.) for the
        // vanilla composition; the mod atmospheres refine it.
        void ComposeLightAttributes(double dayTimeF);
        float m_rainLevel = 0.0f;
        float m_thunderLevel = 0.0f;
        int   m_cameraFluid = 0;
        Game::BiomeId m_cameraBiome = Game::kFallbackBiomeId;
        bool  m_cameraSpectator = false;
        bool  m_cameraResonant  = false;   // eye in resonant water (SetCameraFluid)
        int   m_waterVisionTime = 0;   // MC LocalPlayer.waterVisionTime, 0..600
        int   m_skyFlashTime = 0;      // MC ClientLevel.skyFlashTime (lightning)
        const EnvironmentFrame* m_frameOverride = nullptr;
        // Last UpdateFrame inputs, so FrameForDimension can recompose.
        float     m_lastPartialTick = 0.0f;
        glm::vec3 m_lastCameraForward{0.0f, 0.0f, 1.0f};
        float     m_lastCameraY = 64.0f;
        int       m_lastRenderDistChunks = 8;
        bool      m_lastFogEnabled = true;
        glm::vec3 m_skyboxFogBase{0.5f};
        int m_skyboxMode = 2;

        Atmosphere m_atmosphere = Atmosphere::Vanilla;
        glm::dvec3 m_cameraPos{0.0};
        bool       m_hasCameraPos = false;
        // FrameForDimension composes a dimension the camera is NOT in: no
        // biome sampling around it (the attributes are the dimension's
        // defaults) and no stepping of per-frame state.
        bool       m_composingForeign = false;
        // TF FogHandler.spookyPercent: 0..1, easing toward 1 in the spooky
        // forest and back to 0 outside it.
        float      m_spookyPercent = 0.0f;
        std::chrono::steady_clock::time_point m_lastSpookyStep{};
        bool       m_aetherEternalDay = true;

        EnvironmentFrame m_frame;
    };

} // namespace Render
