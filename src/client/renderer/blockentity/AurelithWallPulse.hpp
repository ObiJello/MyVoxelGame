// File: src/client/renderer/blockentity/AurelithWallPulse.hpp
//
// The slow pulse of light that travels round Aurelith's wall (docs/the-hush.md,
// "Motion and life"): the wall's glowing Stave band (gen_aurelith.py: a ring
// of stave stone one block over the Heart's height, on the octagon between
// Game::Aurelith::kWallIn and kWallOut) carries a travelling swell of light,
// as if the city's outline were still being traced by a held note.
//
// Terrain shaders are not touched: the pulse is drawn as additive, emissive
// quads laid just proud of the band's inner and outer faces (and its top
// where the walk leaves it open), depth-tested, no depth write — the shared
// block-entity shader's glow pipeline. Its position along the circuit is a
// pure function of the level's game time and the Heart's position, so every
// player sees the same pulse.
//
// WHO DRAWS IT. The four voice beacons (VoiceBeaconRenderer): each draws the
// quarter of the circuit nearest its gate (bearings within ±45° of the gate's
// direction from the Heart), so a player anywhere along the wall has a gate
// beacon within the dispatcher's off-screen reach, and the four quarters tile
// the octagon exactly. The gate towers themselves interrupt the band; the
// pulse skips the gate complexes (design |u| <= 19 across the gate axis).
//
// The Heart is derived by the caller from the beacon (its world facing is its
// gate's outward direction; the Heart is kBeaconDistance back along it and
// kBeaconAboveHeart below), so no city record is needed to draw it; the
// awakened brightening reads Client::AurelithState when there is one.
#pragma once

#include <glm/glm.hpp>

namespace Render {

    class AurelithWallPulse {
    public:
        ~AurelithWallPulse();

        // Shader, meshes and texture. False (logged) on failure; Draw is then
        // a no-op.
        bool Initialize();
        void Shutdown();

        // Draw the quarter of the wall pulse belonging to the gate whose
        // world direction from the Heart is `worldDir` (0 north, 1 east,
        // 2 south, 3 west). `lightLevel` is the city's light 0..1
        // (AurelithState::LightLevel; 0 dormant), `voice` its swell 0..1
        // (AurelithState::Voice): a dormant city's pulse is one faint swell
        // at a time; an awakened city's is brighter, quicker and doubled.
        void DrawQuadrant(const glm::ivec3& heart, int worldDir, double ticks,
                          double lightLevel, double voice,
                          const glm::mat4& proj, const glm::mat4& view,
                          const glm::vec3& cameraPos);

    private:
        bool m_initialized = false;
        struct Impl;
        Impl* m_impl = nullptr;
    };

} // namespace Render
