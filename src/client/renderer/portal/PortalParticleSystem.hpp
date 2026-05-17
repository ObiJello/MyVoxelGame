// File: src/client/renderer/portal/PortalParticleSystem.hpp
//
// Tiny focused particle system for the rim sparks Portal portals emit.
// Spawns ~12 particles/sec per active portal at random angular positions
// around the rim. Each particle drifts radially outward with slight
// tangential motion and fades alpha over its ~0.7s lifetime. Drawn as
// camera-facing additive billboards in a single instanced draw per frame.
//
// Self-contained — does NOT plug into a generic particle system because
// we don't have one and building one for this single feature would be
// over-architecting. If a generic system shows up later, this class can
// be the first migration target.
//
// File entirely gated on ENABLE_PORTAL_GUN.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "../backend/RenderTypes.hpp"
#include "../core/Camera.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace Client { class ClientPortalManager; }

namespace Render {

    class PortalParticleSystem {
    public:
        PortalParticleSystem();
        ~PortalParticleSystem();

        bool Initialize();
        void Shutdown();

        // Tick the particle simulation forward. Spawns new particles
        // around active portals, ages existing particles, removes dead
        // ones. Called once per frame from PlatformMain BEFORE Render().
        void Update(float dtSeconds, const Client::ClientPortalManager& mgr);

        // Render every live particle as a camera-facing additive billboard
        // (Portal-game-style energy spark). Uses CPU-built per-frame
        // vertex buffer — particle counts are small (a few dozen across
        // all portals) so per-frame upload is cheap.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos);

        // One-shot burst kinds — matches the reason byte in
        // PortalFizzleS2CPacket. See `portal_X_close` in portals_dump.txt
        // for the close burst's Portal-authoritative parameters
        // (num_to_emit=200, drag=0.25, pull=18000 HU/s², twist=±150 HU/s²,
        // lifetime=1.0s, radius_start_scale=0.35).
        enum class BurstKind : uint8_t {
            BadSurface = 0,   // placement failed on this surface
            Close      = 1,   // portal removed (wall broken / replaced / shift-clear)
        };

        // Spawn a one-shot particle burst at world-space `origin` aligned
        // with `normal`. Used by ClientPortalManager::OnPortalFizzle when
        // a PortalFizzleS2C packet arrives. The two BurstKinds have
        // visually distinct profiles:
        //   • BadSurface — 40 short-lived sparks puffing outward from the
        //     hit point. Conveys "this didn't take."
        //   • Close      — 200 particles vortex-pulled INTO the origin
        //     (Portal's portal_X_close PCF: 18000 HU/s² pull + ±150 HU/s²
        //     twist + drag=0.25, 1s lifetime). Conveys "portal collapsed."
        // `isOrange` selects the per-color palette / twist direction.
        void EmitOneShot(BurstKind kind, const glm::vec3& origin,
                         const glm::vec3& normal, bool isOrange);

        // Spawn a visual portal-gun projectile travelling from `start`
        // to `end`. Speed = 57.15 m/s (Portal's BLAST_SPEED = 3000 HU/s),
        // lifetime = clamp(distance / speed, 0, 0.5s) — Portal's exact
        // `sv_portal_projectile_delay` cap from weapon_portalgun.cpp:82.
        // The visual is a coloured energy bolt (per-color sprite +
        // sprite trail) flying along the straight line gun→impact.
        // Server placement remains instant; this is purely visual.
        void EmitProjectile(const glm::vec3& start, const glm::vec3& end,
                            bool isOrange);

    private:
        // Particle classification — mirrors Portal's two separate continuous
        // effects per portal: `portal_X_particles` (radial sparks, named
        // Spark here) and `portal_X_edge` (tangential rim swirl, Swirl).
        // CloseBurst adds Portal's `portal_X_close` one-shot collapse
        // burst (200 particles pulled inward + twist + drag, 1s lifetime).
        // See `Portal code/sp/src/game/server/portal/prop_portal.cpp:299-300`.
        enum class ParticleType : uint8_t { Spark, Swirl, CloseBurst, Vacuum, Projectile };

        struct Particle {
            glm::vec3 position;     // world-space
            glm::vec3 velocity;     // world-space, m/s
            glm::vec3 colorHot;     // particle's hot color (palette.hot)
            float     age = 0.0f;
            float     lifetime = 0.7f;
            float     birthSizeM = 0.06f;  // initial billboard size in metres
            ParticleType type = ParticleType::Spark;
            // Swirl-only: portal anchor for the vortex force (pull toward
            // origin + twist around normal). Mirrors Portal's
            // "Pull towards control point" + "twist around axis" operators.
            // Ignored for sparks.
            glm::vec3 swirlOrigin{0.0f};
            glm::vec3 swirlNormal{0.0f};
            // Per-particle alpha multiplier (Portal's "Alpha Random" 5-50/255).
            // Sparks use 1.0; swirl uses a low random factor so the ribbon
            // is bright via density, not per-particle alpha.
            float     alphaMul = 1.0f;
            // Trail length in seconds — when rendered, the billboard is
            // stretched backwards along velocity by `trailSec × speed`.
            // Portal's `Trail Length Random` operator: 0.1–0.9 for blue,
            // 0.1–0.75 for orange. Zero = round billboard (sparks).
            float     trailSec = 0.0f;
            // Per-color sprite selection. Portal uses different
            // particle textures for blue (`portal_1_particle.vtf`, 64×64
            // soft glow) and orange (`portal_2_particle.vtf`, 128×128
            // swirly vortex). Splits the render into two draw calls so
            // each batch can bind the right sprite.
            bool      isOrange = false;
            // Per-particle in-plane rotation (radians, set at spawn).
            // Matches Source's "Rotation Random" initializer
            // (rotation_offset_min=0, rotation_offset_max=360 deg in PCF).
            // Used to rotate the billboard quad around the camera-forward
            // axis so each particle's sprite reads differently.
            float     rotation = 0.0f;
        };

        std::vector<Particle> m_particles;

        // Per-pair spawn-rate accumulator so the spawn rate is
        // FPS-independent. Particles emit at a fractional rate per
        // second — the accumulator integrates that until it crosses 1.0,
        // then a particle spawns.
        std::vector<std::pair<uint64_t, float>> m_spawnAccum;
        float& SpawnAccumFor(uint64_t gunId);

        // GPU resources — one shared shader, one streaming vertex
        // buffer rebuilt per frame (small N, no need for instancing /
        // persistent map).
        ShaderHandle  m_shader        = INVALID_SHADER;
        BufferHandle  m_vb            = INVALID_BUFFER;
        MeshHandle    m_mesh          = INVALID_MESH;
        TextureHandle m_dummyTexture  = INVALID_TEXTURE;
        // Portal-extracted sprite textures (assets/textures/portal/).
        // Blue = portal_1_particle.vtf (64×64), Orange = portal_2_particle.vtf
        // (128×128). The vertex layout (pos+uv+color) is unchanged — the
        // fragment shader switches from procedural disc to texture sample
        // when these are bound.
        TextureHandle m_blueSprite    = INVALID_TEXTURE;
        TextureHandle m_orangeSprite  = INVALID_TEXTURE;
        size_t        m_vbCapacityVerts = 0;

        static const char* s_vertSource;
        static const char* s_fragSource;
    };

    extern PortalParticleSystem g_portalParticleSystem;

} // namespace Render

#endif // ENABLE_PORTAL_GUN
