// File: src/client/renderer/environment/HushAtmosphere.hpp
//
// The Hush's atmosphere on top of its fixed night (docs/the-hush.md,
// Atmosphere). EnvironmentState composes the Hush as a constant: the teal
// fog (kHushFog through the skybox override), the pinned night brightness,
// the Hush's sky disc. This layers what CHANGES as you move and as the
// Hush breathes:
//
//   Auroras      Ribbons across the sky (AuroraRenderer). Always faintly
//                there; strong over aurora_steppe. The strength travels in
//                EnvironmentFrame::auroraStrength so a portal's far-side
//                frame carries its own.
//   Cavern fog   Thicker, teal-tinted, shorter fog underground: full in
//                crystal_caverns and hollow_deep, and a weaker general term
//                for being deep under cover anywhere in the Hush.
//   Water        Underwater fog in the Hush pushed toward a luminous teal
//                and brightened; more so in sunken_choir, whose own water
//                fog colour (its biome JSON) still shows through.
//   Stillness    The server's level-wide event (server/level/HushStillness):
//                while it holds, the fog closes in, the auroras dim, the sky
//                flattens toward the fog and the stars fade. It eases in over
//                ~2 s and lifts over ~3 s, whatever the packet timing.
//
// Everything keyed on "where the camera is" is BLENDED over a few seconds
// (the biome at the camera flips at a border; the look must not), stepped
// by wall-clock time so it eases at the same rate at any frame rate and
// however many times a frame the Hush is composed (a portal view composes
// it again).
//
// Only the dimension the camera is IN gets the local terms. A view of the
// Hush through a portal from elsewhere (EnvironmentState::FrameForDimension)
// gets the dimension's defaults: the base aurora, no cavern, no stillness —
// the same rule the Twilight Forest's biome blend follows.
//
// Main (render) thread only.
#pragma once

#include "common/world/biome/Biomes.hpp"

#include <glm/glm.hpp>
#include <chrono>
#include <cstdint>

namespace Render {

    struct EnvironmentFrame;

    class HushAtmosphere {
    public:
        static HushAtmosphere& Get();

        // The camera's level is the Hush. Set with the rest of the dimension
        // sky rules (SkyRenderer::ApplyDimensionSky). Entering snaps the
        // blends to the arrival spot instead of fading from stale values.
        void SetInHush(bool inHush);
        bool InHush() const { return m_inHush; }

        struct Inputs {
            glm::dvec3    cameraPos{0.0};
            bool          hasCameraPos = false;
            int           cameraFluid = 0;     // EnvironmentState::CameraFluid
            Game::BiomeId cameraBiome = Game::kFallbackBiomeId;   // biome at the eye (fluid fog)
            bool          fogEnabled = true;
        };

        // EnvironmentState::UpdateFrame's fixed-night block, after the fluid
        // fog: layers the Hush atmosphere onto `frame` (fog colour and
        // distances, sky colour, star brightness, aurora strength).
        void Compose(EnvironmentFrame& frame, const Inputs& in);

        // 0..1, eased — for debug readouts.
        float Stillness() const;
        float AuroraBiomeBlend() const { return m_steppe; }
        float CavernBlend() const { return m_cavern; }

        // Always-there aurora strength (0..1); aurora_steppe raises it to 1.
        static constexpr float kBaseAurora = 0.3f;

    private:
        HushAtmosphere() = default;

        // Advances the blends and the stillness ease by elapsed time.
        void Step(const Inputs& in);
        // 0..1: how deep under cover the camera is (see the .cpp).
        float SampleDepthTarget(const glm::dvec3& cameraPos) const;

        bool  m_inHush = false;
        bool  m_snap   = true;    // next Step jumps straight to its targets

        // Blends, 0..1, eased toward their targets.
        float m_steppe = 0.0f;    // aurora_steppe
        float m_cavern = 0.0f;    // crystal_caverns / hollow_deep
        float m_choir  = 0.0f;    // sunken_choir
        float m_depth  = 0.0f;    // the general under-cover term

        // The depth probe walks a column of blocks, so it runs a few times
        // a second rather than every frame.
        float m_depthTarget = 0.0f;
        std::chrono::steady_clock::time_point m_lastDepthProbe{};

        // Stillness: the server's on/off as last read, the local deadline
        // that ends it if no "lifted" packet ever comes, and the linear ramp
        // (Stillness() smooths it).
        uint32_t m_stillSerial = 0;
        bool     m_stillOn = false;
        std::chrono::steady_clock::time_point m_stillDeadline{};
        float    m_stillRamp = 0.0f;

        std::chrono::steady_clock::time_point m_lastStep{};
    };

} // namespace Render
