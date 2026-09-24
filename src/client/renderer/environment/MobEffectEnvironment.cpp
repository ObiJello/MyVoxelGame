// File: src/client/renderer/environment/MobEffectEnvironment.cpp
#include "MobEffectEnvironment.hpp"
#include "EnvironmentState.hpp"
#include "client/entity/Player.hpp"
#include "common/entity/effect/MobEffects.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Render {

    namespace {
        MobEffectView g_view;

        float Lerp(float t, float a, float b) { return a + (b - a) * t; }
    }

    void UpdateMobEffectView(const Game::ClientPlayer& player, float partialTick,
                             float darknessEffectScale) {
        MobEffectView v;
        v.spectator = player.IsSpectator();

        if (const Game::MobEffectInstance* blind = player.GetEffect(Game::MobEffectId::Blindness)) {
            v.blindness         = true;
            v.blindnessInfinite = blind->IsInfiniteDuration();
            v.blindnessDuration = blind->duration;
        }

        if (const Game::MobEffectInstance* dark = player.GetEffect(Game::MobEffectId::Darkness)) {
            v.darkness      = true;
            v.darknessBlend = dark->GetBlendFactor(partialTick);
        }
        // LightmapRenderStateExtractor: darknessEffectBrightnessModifier =
        // blend · option, then calculateDarknessScale:
        // max(0, cos((tickCount − partial) · π · 0.025) · 0.45 · modifier) · option.
        {
            const float option = std::clamp(darknessEffectScale, 0.0f, 1.0f);
            const float modifier = player.GetEffectBlendFactor(Game::MobEffectId::Darkness, partialTick) * option;
            const float darkness = 0.45f * modifier;
            const float pulse = std::cos((static_cast<float>(player.tickCount) - partialTick) *
                                         3.1415927f * 0.025f);
            v.darknessLightmap = std::max(0.0f, pulse * darkness) * option;
        }

        // Lightmap night vision: NIGHT_VISION's scale, else CONDUIT_POWER's
        // water vision while submerged.
        if (const Game::MobEffectInstance* nv = player.GetEffect(Game::MobEffectId::NightVision)) {
            v.nightVision = true;
            v.nightVisionIntensity = Game::NightVisionScale(*nv, partialTick);
        } else if (player.HasEffect(Game::MobEffectId::ConduitPower)) {
            const float waterVision = EnvironmentState::Get().WaterVision();
            if (waterVision > 0.0f) v.nightVisionIntensity = waterVision;
        }
        // FogRenderer: brighten only with NIGHT_VISION and without DARKNESS.
        v.nightVisionFogScale = (v.nightVision && !v.darkness) ? v.nightVisionIntensity : 0.0f;

        v.fireResistance = player.HasEffect(Game::MobEffectId::FireResistance);
        g_view = v;
    }

    const MobEffectView& GetMobEffectView() { return g_view; }

    bool MobEffectBlocksSky() { return g_view.blindness || g_view.darkness; }

    void ApplyMobEffectsToFrame(EnvironmentFrame& frame, float renderDistBlocks, int cameraFluid) {
        const MobEffectView& v = g_view;
        const bool inLava = cameraFluid == EnvironmentState::kFluidLava;

        // ── Fog distances: FogRenderer.setupFog takes the FIRST applicable
        //    environment — Lava, PowderSnow, Blindness, Darkness, Water,
        //    Atmospheric. Lava's own distances are EnvironmentState's; only
        //    its FIRE_RESISTANCE branch lives here.
        if (inLava) {
            if (v.fireResistance && !v.spectator) {
                frame.fogEnvStart = 0.0f;           // LavaFogEnvironment
                frame.fogEnvEnd   = 5.0f;
                frame.fogSkyEnd   = frame.fogEnvEnd;
                frame.fogCloudEnd = frame.fogEnvEnd;
            }
        } else if (v.blindness) {
            // BlindnessFogEnvironment.setupFog.
            const float distance = v.blindnessInfinite
                ? 5.0f
                : Lerp(std::min(1.0f, static_cast<float>(v.blindnessDuration) / 20.0f),
                       renderDistBlocks, 5.0f);
            frame.fogEnvStart = distance * 0.25f;
            frame.fogEnvEnd   = distance;
            frame.fogSkyEnd   = distance * 0.8f;
            frame.fogCloudEnd = distance * 0.8f;
        } else if (v.darkness) {
            // DarknessFogEnvironment.setupFog.
            const float distance = Lerp(v.darknessBlend, renderDistBlocks, 15.0f);
            frame.fogEnvStart = distance * 0.75f;
            frame.fogEnvEnd   = distance;
            frame.fogSkyEnd   = distance;
            frame.fogCloudEnd = distance;
        }

        // ── Fog colour: FogRenderer.computeFogColor's darkness and
        //    night-vision passes. The first darkness-modifying environment is
        //    Blindness, then Darkness; the void darkness it starts from is 0
        //    here (the engine has no void-darkness onset).
        float darkness = 0.0f;
        if (v.blindness) {
            // BlindnessFogEnvironment.getModifiedDarkness.
            darkness = (!v.blindnessInfinite && v.blindnessDuration <= 19)
                ? std::max(static_cast<float>(v.blindnessDuration) / 20.0f, darkness)
                : 1.0f;
        } else if (v.darkness) {
            // DarknessFogEnvironment.getModifiedDarkness.
            darkness = std::max(v.darknessBlend, darkness);
        }
        glm::vec3 fog = frame.fogColor;
        if (darkness > 0.0f && !inLava) {
            const float brighten = (1.0f - darkness) * (1.0f - darkness);
            fog *= brighten;
        }
        // Night vision brightens the fog colour except in water (where MC
        // uses the water vision instead — EnvironmentState's business).
        if (cameraFluid != EnvironmentState::kFluidWater && v.nightVisionFogScale > 0.0f &&
            fog.r != 0.0f && fog.g != 0.0f && fog.b != 0.0f) {
            const float targetScale = 1.0f / std::max(fog.r, std::max(fog.g, fog.b));
            fog = glm::mix(fog, fog * targetScale, v.nightVisionFogScale);
        }
        frame.fogColor = fog;

        // ── The lightmap's two effect terms, on the one global light knob ──
        if (v.nightVisionIntensity > 0.0f) {
            frame.skyBrightness = Lerp(v.nightVisionIntensity, frame.skyBrightness, 1.0f);
        }
        if (v.darknessLightmap > 0.0f) {
            frame.skyBrightness = std::max(0.0f, frame.skyBrightness - v.darknessLightmap);
        }
    }

    glm::mat4 MakeNauseaWarp(float nauseaIntensity, float screenEffectScale, float angleDegrees) {
        // GameRenderer.renderLevel: spinningEffectIntensity = max(portal,
        // nausea) · screenEffectScale² (the portal term is the nether
        // portal's own effect, not ported with this).
        const float intensity = nauseaIntensity * screenEffectScale * screenEffectScale;
        if (!(intensity > 0.0f)) return glm::mat4(1.0f);
        float skew = 5.0f / (intensity * intensity + 5.0f) - intensity * 0.04f;
        skew *= skew;
        const glm::vec3 axis(0.0f, 0.70710677f, 0.70710677f);   // (0, √2/2, √2/2)
        const float angle = angleDegrees * 0.017453292f;
        glm::mat4 w = glm::rotate(glm::mat4(1.0f), angle, axis);
        w = glm::scale(w, glm::vec3(1.0f / skew, 1.0f, 1.0f));
        w = glm::rotate(w, -angle, axis);
        return w;
    }

} // namespace Render
