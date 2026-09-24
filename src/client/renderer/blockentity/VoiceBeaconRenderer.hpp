// File: src/client/renderer/blockentity/VoiceBeaconRenderer.hpp
//
// Aurelith's gate beams (docs/hush-lore.md, "The Four Gates"): each of the
// city's four gate towers is crowned with a voice beacon
// (BlockEntityTypeIds::VOICE_BEACON), and this renderer stands its light up
// into the sky — a slender column visible from far across the Hush, so a
// traveller who crests a hill in the dark sees four lines of light and knows
// where the Lantern City is.
//
// ── The voices ───────────────────────────────────────────────────────────
// The block's `facing` picks the voice (and so the colour):
//   north  Soprano  pale cyan-white  #DFFFFF
//   east   Alto     cyan             #5FF3FF
//   south  Tenor    violet           #B77CFF
//   west   Bass     indigo-blue      #6F7BFF
// A beacon read from a view whose level is not the local one (a portal
// view) falls back to a voice seeded from its position.
//
// ── The beam ─────────────────────────────────────────────────────────────
// A volumetric column (Render::VolumetricBeam, effects/): light scattered by
// the Hush's haze, rising kBeamHeight blocks from the lens — a hot core in
// the voice's colour washed toward white inside a wider falloff in the
// voice's colour, drifting mist, fading over its last stretch. Its
// fogFactor is below 1, so the beam stays readable past the fog: a landmark
// that leads travellers to the city. Past 96 blocks its radius widens with
// distance (MC BeaconRenderer's beamRadiusScale) so a far one stays a visible
// line. A cached axis raycast ends it under anything built over the lens; it
// casts no pool (it points at the sky). A glare sprite sits on the lens.
//
// Every few seconds a faint ring of light rises up the column — the note
// travelling up the voice — drawn as additive annuli with the shared
// block-entity shader, and the light breathes. All of it is a pure function
// of the level's game time and a seed from the block's position, so every
// player sees the same pulses and no two beacons run in step.
//
// ── The awakening (docs/the-hush.md "Reawakening the Heart") ─────────────
// A beacon knows its city from itself: its world facing is its gate's
// outward direction, so the Heart is Game::Aurelith::kBeaconDistance back
// along it and kBeaconAboveHeart below. With the city's record (Client::
// AurelithState) the voice comes from the gate's DESIGN direction (the
// template rotation turns the facing, which would otherwise name the wrong
// voice in a turned city), and as the Chord is sung the beam bends: a
// quadratic arc from the lens, up and then inward, to the point over the
// Heart where the four meet (kConvergeAboveHeart) and the Heart's own pillar
// carries on — drawn as a chain of short VolumetricBeam columns along the
// arc. Under the Undersong the four run dark toward violet.
//
// The wall's Stave band carries a travelling pulse of light; each beacon
// draws the quarter of it nearest its gate (AurelithWallPulse).
//
// ── Off-screen ───────────────────────────────────────────────────────────
// ShouldRenderOffScreen: the beam must show while the tower-top section is
// culled or occluded (the city behind a hill, the beam above it). The
// dispatcher's off-screen walk covers GetOffScreenReach() blocks round the
// camera; inside the visible pass the beam draws as far as the view reaches.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "AurelithRenderCommon.hpp"
#include "AurelithWallPulse.hpp"
#include "../backend/RenderTypes.hpp"

#include <array>
#include <cstdint>

namespace Render {

    class VoiceBeaconRenderer : public BlockEntityRenderer {
    public:
        ~VoiceBeaconRenderer() override;

        // Creates the shader, bakes the per-voice pulse rings and their
        // texture, and acquires the shared volumetric-beam module (the pulses
        // still draw if that fails). False (logged) on failure.
        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

        // A landmark: as far as the view reaches (the fog ends it first).
        int  GetViewDistance() const override { return kViewDistance; }
        bool ShouldRenderOffScreen() const override { return true; }
        int  GetOffScreenReach() const override { return kOffScreenReach; }

        static constexpr int   kVoiceCount     = 4;       // soprano, alto, tenor, bass
        static constexpr float kBeamHeight     = 256.0f;  // blocks above the lens
        static constexpr int   kViewDistance   = 1024;
        static constexpr int   kOffScreenReach = 160;

    private:
        // The column of light and the lens glare (VolumetricBeam). `base` is
        // the lens's top face in world space.
        // `bend` 0..1 bends the beam from straight up to the arc ending at
        // `meet` (the convergence point over the Heart); `sour` darkens it.
        void DrawSkyBeam(int voice, const glm::ivec3& pos, const glm::dvec3& base,
                         float widen, float light, double bend, const glm::dvec3& meet, double sour,
                         const glm::mat4& proj, const glm::mat4& view);

        bool          m_initialized = false;
        bool          m_beamAcquired = false;   // VolumetricBeam::Acquire succeeded
        ShaderHandle  m_shader = INVALID_SHADER;
        std::array<Aurelith::GpuMesh, kVoiceCount> m_pulse;   // unit-radius annulus
        TextureHandle m_bellTex = INVALID_TEXTURE;
        AurelithWallPulse m_wallPulse;
        bool m_wallPulseReady = false;
    };

} // namespace Render
