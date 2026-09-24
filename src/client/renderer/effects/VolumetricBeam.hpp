// File: src/client/renderer/effects/VolumetricBeam.hpp
//
// Volumetric light beams: cones of light you see IN the air — a lighthouse
// sweep, a searchlight, a sky beam — drawn as light scattered by the haze
// along each view ray, not as textured planes. Any renderer can call it; the
// Hush lighthouse (HushLighthouseRenderer) is the first user. docs/
// volumetric-light.md has the walkthrough for new callers.
//
// ── The model (shaders/beam_volume.frag) ────────────────────────────────
// Each cone has an apex, a unit axis D, a length L and a radius growing
// linearly from r0 at the apex to r1 at L. A proxy mesh — a 16-sided capped
// frustum a little larger than the cone — is rasterised; every fragment
// intersects its view ray x(t) = eye + t·R with the cone analytically
// (|x−A|² − s² ≤ (r0 + k·s)², s = (x−A)·D, k = (r1−r0)/L: a quadratic in t,
// cut to the slab 0 ≤ s ≤ L) and integrates the in-scattered light over the
// resulting segment [tNear, tFar] with up to six jittered samples:
//
//   Lin = Σ radial(ρ) · axial(s) · phase(μ) · mist(x) · T(x) · Δt · haze · I
//
//   radial(ρ)  ρ = distance from the axis / r(s): a tight core (core colour,
//              e^−14ρ²) inside a wide falloff (edge colour, e^−3ρ²), windowed
//              by (1−ρ²)² so the rim fades to exactly nothing
//   axial(s)   fade in over `fadeIn` blocks from the lens, the inverse-square
//              spread 1/(1 + (s/halfIntensityDistance)²), extinction e^−σs,
//              and a fade over the last part of the length
//   phase(μ)   Henyey–Greenstein with forward scattering (g ≈ 0.6–0.7),
//              normalised to 1 at a side view: μ = cos between the light's
//              direction (away from the apex) and the direction to the eye,
//              so looking back at the lamp through the beam glows 20–40×
//              brighter than the shaft seen side-on
//   mist(x)    two octaves of drifting 3D value noise (a 64² texture with the
//              "z-slices are an offset" trick — one fetch per octave)
//   T(x)       1 − the terrain's own fog at x (spherical + cylindrical linear
//              fog, EntityEnvironment's), so a far beam fades into the fog at
//              exactly the rate the hill behind it does
//
// then 1 − e^−Lin (a soft shoulder instead of a clipped white), added to the
// frame (blend ONE, ONE; depth test per the mode below, never a depth write).
// Denser atmospheric fog makes the beam MORE visible: the haze density and
// extinction scale with the frame's environmental fog (cavern fog, the
// Hush's stillness, water) — see EnvironmentDensity in the .cpp.
//
// ── Scene intersection, per backend ─────────────────────────────────────
//   OpenGL   the default framebuffer's depth is copied once per view into a
//            depth texture (RenderBackend::CopyFramebufferDepthToTexture) the
//            first time a beam draws in it; the shader ends each ray at the
//            scene (exact, soft — no line where the beam meets a hill), the
//            proxy draws its BACK faces with the depth test off, and the
//            light pool becomes a deferred decal lighting the true terrain.
//            OBEY_BEAM_DEPTH=0 turns the copy off (A/B).
//   Vulkan   the frame pass does not store depth (VKBackend: DONT_CARE), so
//   (and GL  there is nothing to sample. Outside the proxy the FRONT faces
//   without  draw depth-tested — terrain in front of the beam hides it; from
//   depth)   inside, the back faces draw with the test off. A CPU raycast
//            along the axis (below) ends the cone softly where it meets
//            terrain, so a beam aimed into a hill stops at the hill.
//
// ── Where the beam lands ────────────────────────────────────────────────
// There is no block-light engine, so this is the one way a beam touches the
// world. A DDA raycast along each cone's axis against the client block
// access (full opaque cubes stop it; glass, leaves and water do not) finds
// the hit — re-run when the beam moves, otherwise every few views (`cacheKey`).
// There a LIGHT POOL relights the terrain multiplicatively (blend DST_COLOR,
// ONE: dst·(1 + light)), so the block textures show through the splash: on
// OpenGL a box decal lights the real scene points inside it with the beam's
// analytic light (Lambert off the depth-derived normal); without depth, a
// quad in the plane of the face that was hit.
//
// ── Glare ───────────────────────────────────────────────────────────────
// BeamGlare: a camera-facing sprite at a source — a hot core, a soft halo and
// an anamorphic horizontal streak — for "the lens you are looking into". The
// caller supplies its flare (Flare() below: how directly a cone points at the
// eye). On OpenGL it is occlusion-tested against the depth snapshot (five
// taps round the source), so the streak spills over nearby geometry the way
// real glare does; without depth it is depth-tested.
//
// ── Cost ────────────────────────────────────────────────────────────────
// One proxy draw per cone (64 triangles), one pool and one glare draw; the
// fragment work is ≤ 6 samples × (2 noise fetches + a few exp). Cones out of
// the frustum or wholly past the fog's end are skipped on the CPU.
//
// ── Threading / lifetime ────────────────────────────────────────────────
// Main (render) thread only. Resources are shared and reference-counted:
// each user calls Acquire() when it initialises and Release() when it shuts
// down. BeginView() marks a new view (the depth snapshot is per view): the
// block-entity dispatcher calls it at the top of every RenderAll; a caller
// drawing beams from another pass calls it once before its first Draw.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"

#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace Render {

    // One cone of light. World-space position, everything else in blocks.
    struct BeamCone {
        glm::dvec3 apex{0.0};                  // the lens: where the light leaves
        glm::vec3  direction{0.0f, 1.0f, 0.0f};// unit axis (normalised on use)
        float length      = 32.0f;             // blocks along the axis
        float startRadius = 0.25f;             // at the apex
        float endRadius   = 3.0f;              // at `length` (≥ startRadius; equal = a column)

        glm::vec3 coreColor{1.0f};             // the hot centre
        glm::vec3 edgeColor{1.0f};             // the wide falloff round it
        float intensity = 1.0f;                // overall brightness (flicker goes here)

        float haze        = 0.35f;             // scattering density of the air, per block
        float extinction  = 0.01f;             // light lost along the beam, per block
        float anisotropy  = 0.65f;             // Henyey–Greenstein g (forward scattering)
        float mist        = 0.5f;              // 0..1: how much the drifting noise modulates it
        uint32_t seed     = 0;                 // decorrelates one beam's mist from another's

        float fadeIn                = 1.5f;    // blocks from the apex over which it grows in
        float halfIntensityDistance = 24.0f;   // inverse-square-ish: half as bright here
        float fadeOutStart          = 0.45f;   // fraction of `length` where the far fade begins
        float fogFactor             = 1.0f;    // 1: fogged like terrain; 0: ignores the fog

        // Terrain (a CPU raycast along the axis).
        bool  terrainClip   = true;            // end the cone where the axis meets terrain
        bool  lightPool     = true;            // relight the terrain where it lands
        float terrainStart  = 0.0f;            // ignore blocks nearer than this (the lamp's housing)
        float poolIntensity = 1.0f;
        // Stable identity for the raycast cache (e.g. a hash of the block
        // position and the beam's index): the hit is re-used while the
        // beam holds still. 0 = raycast on every Draw.
        uint64_t cacheKey = 0;
    };

    // A glare sprite at a light source.
    struct BeamGlare {
        glm::dvec3 position{0.0};              // world; put it just in front of the lens
        glm::vec3  color{1.0f};
        float size      = 1.5f;                // halo radius in blocks
        float intensity = 1.0f;
        float streak    = 0.5f;                // anamorphic streak strength (0 = none)
        float streakLength = 4.0f;             // streak half-length, in halo radii
        float fogFactor = 1.0f;
    };

    class VolumetricBeam {
    public:
        static VolumetricBeam& Get();

        // Reference-counted GPU resources (shaders, proxy meshes, the noise
        // texture, the depth snapshot). Acquire creates them on the first
        // call (false, logged, if that fails); the last Release frees them.
        static bool Acquire();
        static void Release();

        // A new view is about to draw (see the header comment). Cheap.
        void BeginView();

        // Draws the pools, then the cones, then the glares, into the current
        // view. `proj` and `view` are the view's matrices (RENDER space —
        // RenderOrigin.hpp — as every renderer receives them). Leaves the
        // pipeline state and bound shader changed, like any draw.
        void Draw(const BeamCone* cones, size_t coneCount,
                  const BeamGlare* glares, size_t glareCount,
                  const glm::mat4& proj, const glm::mat4& view);

        // The eye of a render-space view matrix, in WORLD space — exact,
        // unlike the float camera position the block-entity renderers get.
        static glm::dvec3 EyeFromView(const glm::mat4& view);

        // 0..1: how directly `cone` points at `eye` — 1 within `innerDeg` of
        // its axis, 0 past `outerDeg`, smooth between. For BeamGlare.
        static float Flare(const BeamCone& cone, const glm::dvec3& eye,
                           float innerDeg = 2.5f, float outerDeg = 16.0f);

    private:
        VolumetricBeam() = default;

        bool Initialize();
        void Shutdown();

        struct Mesh {
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            uint32_t     indexCount = 0;
        };
        static bool BuildMesh(Mesh& out, const void* verts, size_t vertBytes,
                              const uint32_t* indices, size_t indexCount);
        static void DestroyMesh(Mesh& m);

        // Where the axis meets terrain, in blocks along it (hit == false: the
        // whole length is clear).
        struct TerrainHit {
            bool       hit = false;
            float      s = 0.0f;               // distance along the axis
            glm::dvec3 point{0.0};             // world
            glm::vec3  normal{0.0f};           // the face entered
        };
        TerrainHit ProbeTerrain(const BeamCone& cone, const glm::vec3& dir);

        // The depth snapshot for the current view (OpenGL). False: none.
        bool EnsureSceneDepth(const glm::mat4& viewProj);

        bool          m_initialized = false;
        int           m_refs = 0;
        ShaderHandle  m_volumeShader = INVALID_SHADER;
        ShaderHandle  m_glareShader  = INVALID_SHADER;
        ShaderHandle  m_poolShader   = INVALID_SHADER;
        Mesh          m_proxy;                 // unit capped frustum, 16 sides
        Mesh          m_cube;                  // [-1,1]³, outward CCW
        Mesh          m_quad;                  // [-1,1]² at z = 0
        TextureHandle m_noiseTex = INVALID_TEXTURE;

        // Per-view bookkeeping.
        uint32_t      m_pass = 1;

        // The depth snapshot (OpenGL).
        TextureHandle m_depthTex = INVALID_TEXTURE;
        int           m_depthW = 0, m_depthH = 0;
        uint32_t      m_depthPass = 0;
        glm::mat4     m_depthViewProj{0.0f};
        bool          m_depthValid = false;
        bool          m_depthDisabled = false;  // OBEY_BEAM_DEPTH=0

        // The terrain-probe cache (cacheKey → last probe).
        struct ProbeEntry {
            glm::dvec3 apex{0.0};
            glm::vec3  dir{0.0f};
            float      length = 0.0f;
            float      start = 0.0f;
            uint32_t   pass = 0;
            TerrainHit result;
        };
        std::unordered_map<uint64_t, ProbeEntry> m_probes;
    };

} // namespace Render
