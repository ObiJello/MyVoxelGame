// File: src/client/renderer/blockentity/HushLighthouseRenderer.hpp
//
// The Hush lighthouse lamp's sweep (docs/the-hush.md, "Hush Lighthouses"):
// two opposed, slightly downward beams turning slowly over the landscape,
// and a glow on the lens that flares when a beam points at the viewer.
// Hangs off the lamp's block entity (BlockEntityTypeIds::HUSH_LIGHTHOUSE_
// LAMP) the way MC's BeaconRenderer hangs off the beacon's.
//
// ── The look (Render::VolumetricBeam) ────────────────────────────────────
// Each beam is a volumetric cone of light (effects/VolumetricBeam.hpp): light
// scattered by the Hush's haze along every view ray, a pale cyan core in a
// teal falloff, brightest looking back down the beam at the lens (forward
// scattering), drifting mist inside it, fogged exactly as the terrain behind
// it is, and — the Hush's fog being thick — denser where the fog is. Where a
// beam grazes a hill it stops at the hill and throws a pool of light on it;
// the lens glares, flaring when a beam points at the viewer. The lantern
// room's cyan glass is skipped by the terrain probe (terrainStart). Past 96
// blocks the beams widen with distance (MC BeaconRenderer's beamRadiusScale)
// so a far one stays readable.
//
// ── The sweep (a pure function of game time and the lamp's position) ────
// The angle comes from the LEVEL's game time (EnvironmentState::GameTimeF,
// the server's clock the client mirrors — the Hush's day time is fixed, its
// game time is not), never the frame clock, so every player sees the same
// sweep; a seed from the lamp's position (MC Mth.getSeed) gives each lamp
// its own period (a quarter turn every 7.5..10.5 s: a full turn in 30..42 s),
// phase and direction, so neighbouring lighthouses never run in step.
//
// It does not spin evenly. Each quarter turn is a STROKE: a seeded pause,
// the move eased in and out (a sigmoid of seeded steepness), a seeded pause
// at the end, and in most strokes a hitch — the mechanism stalls part-way,
// often shuddering back a degree or two before it catches. The light
// breathes and shimmers and, now and then, gutters for a moment.
//
// ── It notices you ───────────────────────────────────────────────────────
// Per viewer (client-side, off the local player): when a beam's sweep comes
// round to the player (within ~56 blocks) it slows, stops on them, dips to
// their eyes and steadies its light for about a second and a half, then
// swings off faster to make the time up. Built as a monotone warp of the
// base angle around the player's bearing, blended in by distance, so the
// beam never jumps — not when the player walks into range, not when they
// run across the beam. Portal views draw the plain sweep (the player is not
// in the view's world there).
//
// ── It guides ────────────────────────────────────────────────────────────
// A lamp within 600 blocks of an Aurelith knows where the city's Heart is
// (its block entity, HushLighthouseLampBlockEntity, filled in once by the
// server's LighthouseGuide). In one turn of every four to six (seeded per
// lamp), the first stroke that carries a beam across the bearing to the city
// splits its move there: the beam eases onto the city, holds on it for about
// three and a half seconds — its light steadied and a little brighter, the
// beam levelled toward the horizon — and eases on to finish the stroke on
// time. Still a pure function of game time, the lamp and its target, so
// every player sees the same pause; the noticing warp runs on top of it.
//
// ── Off-screen ───────────────────────────────────────────────────────────
// ShouldRenderOffScreen (MC's, true for the beacon): the dispatcher draws the
// lamp while its section is culled, and walks the chunks within
// GetOffScreenReach() of the camera for lamps no visible section brought
// in — look away from the tower and its beam still sweeps over you.
#pragma once

#include "BlockEntityRenderer.hpp"
#include <cstdint>

namespace Render {

    class HushLighthouseRenderer : public BlockEntityRenderer {
    public:
        ~HushLighthouseRenderer() override;

        // Takes a reference on the shared volumetric-beam resources.
        // Returns false (having logged) on failure.
        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

        // A landmark: visible as far as the view reaches (MC's beacon returns
        // the render distance; the fog ends the beams before this does).
        int  GetViewDistance() const override { return kViewDistance; }
        bool ShouldRenderOffScreen() const override { return true; }
        // A beam's length plus its far half-width, and a margin.
        int  GetOffScreenReach() const override { return kOffScreenReach; }

        // Beam geometry and reach, in blocks.
        static constexpr float kBeamLength    = 60.0f;
        static constexpr float kBeamStart     = 0.40f;   // just outside the lens
        static constexpr int   kViewDistance  = 512;
        static constexpr int   kOffScreenReach = 72;

    private:
        bool m_initialized = false;
    };

} // namespace Render
