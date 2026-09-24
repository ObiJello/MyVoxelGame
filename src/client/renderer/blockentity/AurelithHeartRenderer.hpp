// File: src/client/renderer/blockentity/AurelithHeartRenderer.hpp
//
// The Heart of Aurelith (docs/hush-lore.md): the Choir's silenced resonance
// engine at the centre of the Plaza of the Held Note. Its core block
// (resonance_engine, BlockEntityTypeIds::RESONANCE_ENGINE) is ordinary chunk
// geometry; this renderer draws what hangs above it — four enormous rings of
// resonant crystal turning slowly in the air round a glowing core, still
// keeping time a thousand years after the Chord stopped.
//
// ── The rings ────────────────────────────────────────────────────────────
// Four rings centred kCoreHeight blocks above the engine block, radii 2.6,
// 5, 8 and 12 blocks. Each is built of faceted crystal STAVES — hexagonal
// prisms tapering to points, laid end to end round the circle with a gap
// between each (Choir engineering, not a smooth torus) — a few staves
// missing or snapped short, and violet crystal nodes jutting from every
// third stave. Every ring hangs tilted on its own axis (40, 8, 23 and 52
// degrees), the tilt axes precessing very slowly, and each ring turns in
// its own plane.
//
// ── The turning (a pure function of time and position) ───────────────────
// Like the lighthouse sweep: every ring turns in STROKES of a twelfth of a
// turn, each stroke a short pause, a seeded ease and, in about a third of
// them, a stall part-way with a shudder of a degree or so before it
// catches. Periods are 41..117 s per turn, seeded ±10 % from the engine's
// position; directions alternate ring to ring. All of it is keyed on the
// level's game time (EnvironmentState::GameTimeF), so every player sees the
// same rings.
//
// ── It notices you ───────────────────────────────────────────────────────
// Per viewer (client-side, off the local player): within ~24 blocks
// (horizontally) of the engine the rings slow almost to a stop over about
// two seconds, the core brightens and one slow pulse of light rolls outward
// through the rings, lighting each as it passes. Walk away and the rings
// ease back up — a little faster than their own pace until they have made
// up the time they lost, so they rejoin the shared clock without a jump.
// The viewer's own clock (tau) is integrated per engine with an eased
// speed, so the motion is C1 throughout. Portal views draw the plain
// animation on the shared clock.
//
// ── Light ────────────────────────────────────────────────────────────────
// The crystal is drawn opaque (depth-writing, emissive, fogged — the shared
// block-entity shader), with facet shading baked into the vertices. The
// glow — a halo round each ring, the core's billboard and the notice pulse —
// is additive, with no depth write. The light itself is volumetric
// (Render::VolumetricBeam, effects/): a soft column of cyan-violet haze
// rising from the engine up through the rings, and a faint cone falling from
// the core onto the dais that leaves a pool of light round the engine. Both
// brighten while the Heart watches you; both live in DrawLightShaft.
//
// ── Reawakened (docs/the-hush.md "Reawakening the Heart") ────────────────
// The city record (Client::AurelithState) drives the rest, on the level's
// game time: through the awakening the rings spin up (their clock integrates
// AurelithState::RingPace — the "shared clock" below is that integral, so
// every player's rings still agree), the core and halos swell with the
// Chord, a pillar of light rises out of the rings to where the four gate
// beams meet (Game::Aurelith::kConvergeAboveHeart); under the Undersong the
// rings stutter and the light gutters and runs violet; once awakened they
// turn steadily at three times their old pace, and the resolution flashes
// out through them. A waking Heart does not stop to watch anyone.
//
// ── Off-screen ───────────────────────────────────────────────────────────
// ShouldRenderOffScreen: the rings reach 12 blocks out and 20 up from the
// block, so they draw while the engine's own section is culled, and are
// visible from the city's edge (GetViewDistance well past it).
#pragma once

#include "BlockEntityRenderer.hpp"
#include "AurelithRenderCommon.hpp"
#include "../backend/RenderTypes.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace Render {

    class AurelithHeartRenderer : public BlockEntityRenderer {
    public:
        ~AurelithHeartRenderer() override;

        // Creates the shader, bakes the ring, core, glow and pulse meshes and
        // their procedural textures, and acquires the shared volumetric-beam
        // module (the rings still draw if that fails). False (logged) on
        // failure.
        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

        // The city is ~200 blocks across; the Heart must read from its edge
        // and beyond (the fog ends it first).
        int  GetViewDistance() const override { return kViewDistance; }
        bool ShouldRenderOffScreen() const override { return true; }
        int  GetOffScreenReach() const override { return kOffScreenReach; }

        static constexpr int    kRingCount      = 4;
        static constexpr float  kCoreHeight     = 11.5f;   // ring centre above the block centre
        static constexpr int    kViewDistance   = 384;
        // 160 (the gate beacons' reach): the awakened pillar stands over the
        // city, and must not vanish while the plaza's section is culled.
        static constexpr int    kOffScreenReach = 160;

    private:
        // One viewer's clock for one engine (see "It notices you").
        struct ViewState {
            double tau        = 0.0;   // the viewer's ring clock, in ticks
            double lastTicks  = 0.0;   // game time at the last update
            double speed      = 1.0;   // d(tau)/d(ticks), eased
            double notice     = 0.0;   // 0..1, eased
            double pulseStart = -1e9;  // game time the last pulse began
            bool   init       = false;
        };
        // `sharedClock` is where every player's rings should be (the
        // integral of the city's ring pace), `pace` its rate now.
        ViewState& UpdateViewState(const glm::ivec3& pos, double ticks, double sharedClock, double pace,
                                   bool viewerNear, double nearAmount);

        // The Heart's volumetric light: the column from the engine up
        // through the rings and the cone from the core down onto the dais
        // (with its light pool). `light` is the breathing brightness,
        // `notice` how hard the Heart is watching the viewer.
        // `voice`, `sour` and `pillar` are the city's AurelithState Voice,
        // Sourness and BeamConvergence (0 for a dormant or unknown city).
        void DrawLightShaft(const glm::ivec3& pos, const glm::dvec3& blockCentre,
                            double light, double notice, double voice, double sour, double pillar,
                            const glm::mat4& proj, const glm::mat4& view);

        bool          m_initialized = false;
        bool          m_beamAcquired = false;   // VolumetricBeam::Acquire succeeded
        ShaderHandle  m_shader = INVALID_SHADER;
        std::array<Aurelith::GpuMesh, kRingCount> m_rings;      // crystal staves + nodes, XZ plane
        std::array<Aurelith::GpuMesh, kRingCount> m_ringGlow;   // halo annulus + edge-on band
        Aurelith::GpuMesh m_core;      // the core crystal (a hexagonal bipyramid)
        Aurelith::GpuMesh m_billboard; // unit glow quad
        Aurelith::GpuMesh m_pulse;     // unit-radius annulus (the notice pulse)
        TextureHandle m_crystalTex = INVALID_TEXTURE;
        TextureHandle m_bellTex    = INVALID_TEXTURE;
        TextureHandle m_glowTex    = INVALID_TEXTURE;

        std::unordered_map<uint64_t, ViewState> m_views;
    };

} // namespace Render
