// File: src/client/renderer/environment/MobEffectEnvironment.hpp
//
// What the local player's status effects do to the view — the renderer half
// of the effect port:
//
//   BLINDNESS   MC BlindnessFogEnvironment: fog closing in to 5 blocks (over
//               the first second), the fog colour pulled to black, the sky
//               not drawn (Camera.doesMobEffectBlockSky).
//   DARKNESS    MC DarknessFogEnvironment: fog at 15 blocks eased by the
//               effect's 22-tick blend, the same colour darkening and sky
//               block, and LightmapRenderStateExtractor's pulsing
//               darknessEffectScale (0.45 · blend · cos(tick · π · 0.025)).
//   NIGHT_VISION LightmapRenderStateExtractor.nightVisionEffectIntensity
//               (GameRenderer.nightVisionScale — steady, flickering in the
//               last 10 s) and FogRenderer's fog-colour brighten. CONDUIT_POWER
//               gives the same intensity underwater (the water vision).
//   FIRE_RESISTANCE MC LavaFogEnvironment: 0..5 blocks of lava fog instead of
//               0.25..1.
//   NAUSEA      GameRenderer's projection warp (see MakeNauseaWarp).
//
// NO LIGHTMAP IN THIS ENGINE: light is baked into the terrain vertex colour
// and the only global light knob is EnvironmentFrame::skyBrightness (the
// uSkyBrightness terrain/entity dim). The lightmap effects are therefore
// applied there: NIGHT_VISION lerps skyBrightness toward 1 by its intensity
// (MC max(ambient, nightVisionColor · intensity) lifts every light level;
// here the day/night dim goes away but a cave's baked darkness does not), and
// DARKNESS subtracts its pulsing scale from it (MC `color - DarknessScale`
// on the lightmap), which dims everything the uniform dims.
#pragma once

#include <glm/glm.hpp>

namespace Game { class ClientPlayer; }

namespace Render {

    struct EnvironmentFrame;

    // The per-frame inputs, from the local player (UpdateMobEffectView).
    struct MobEffectView {
        bool  blindness            = false;
        bool  blindnessInfinite    = false;
        int   blindnessDuration    = 0;     // ticks left
        bool  darkness             = false;
        float darknessBlend        = 0.0f;  // BlendState factor, partial-tick lerped
        float darknessLightmap     = 0.0f;  // calculateDarknessScale · option
        bool  nightVision          = false; // MC hasEffect(NIGHT_VISION)
        float nightVisionIntensity = 0.0f;  // lightmap intensity (NV or conduit water vision)
        float nightVisionFogScale  = 0.0f;  // FogRenderer brighten (NV without DARKNESS)
        bool  fireResistance       = false;
        bool  spectator            = false;
    };

    // Called by the host once per frame, before EnvironmentState::UpdateFrame.
    // `darknessEffectScale` is the Darkness Pulsing accessibility option.
    void UpdateMobEffectView(const Game::ClientPlayer& player, float partialTick,
                             float darknessEffectScale);
    const MobEffectView& GetMobEffectView();

    // MC FogRenderer (Blindness / Darkness / the lava fire-resistance case,
    // and the colour's darkness + night-vision passes) and the lightmap's
    // night vision / darkness, applied to a composed frame. Run by
    // EnvironmentState::UpdateFrame last, after the fluid fog and every
    // dimension rule. `cameraFluid` is EnvironmentState::CameraFluid.
    void ApplyMobEffectsToFrame(EnvironmentFrame& frame, float renderDistBlocks, int cameraFluid);

    // MC Camera.doesMobEffectBlockSky: BLINDNESS or DARKNESS — the sky pass
    // is skipped and the clear colour (the darkened fog) shows instead.
    bool MobEffectBlocksSky();

    // MC GameRenderer's spinning effect, as the view-space matrix that sits
    // between the damage tilt and the camera (P · bobHurt · W · V):
    // W = R(angle, (0, √½, √½)) · S(1/skew, 1, 1) · R(-angle), with
    // intensity = nausea blend · screenEffectScale², skew = (5/(i²+5) −
    // 0.04·i)². Identity when the intensity is 0.
    glm::mat4 MakeNauseaWarp(float nauseaIntensity, float screenEffectScale, float angleDegrees);

} // namespace Render
