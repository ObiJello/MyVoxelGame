// File: src/client/renderer/portal/PortalParticleSystem.cpp
// See header for high-level scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalParticleSystem.hpp"
#include "../backend/RenderBackend.hpp"
#include "client/portal/ClientPortalManager.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Render {

    PortalParticleSystem g_portalParticleSystem;

    // Sparks — DISABLED to match Portal. portal_X_particles has
    // emission_rate=0 in the PCF (verified portals_dump.txt:845-853);
    // the actual visible particles around active portals come from a
    // child Portal_X_vacuum effect (not yet implemented). Leaving the
    // spawn rate at 0 removes our incorrect outward sparks until the
    // vacuum cloud is ported.
    static constexpr float kSpawnRatePerPortal     = 0.0f;   // PCF: emission_rate = 0
    [[maybe_unused]] static constexpr float kLegacySparkRate = 16.0f; // kept for reference
    static constexpr float kBaseLifetime           = 0.65f;  // seconds, ± jitter
    static constexpr float kLifetimeJitter         = 0.20f;
    static constexpr float kRadialSpeed            = 0.45f;  // m/s outward from rim
    static constexpr float kTangentialSpeedJitter  = 0.10f;  // m/s lateral
    static constexpr float kVerticalDrift          = 0.08f;  // m/s upward (warm-air feel)
    static constexpr float kBirthSize              = 0.055f; // metres
    static constexpr float kPortalWidthHalf        = 0.5f;       // matches PortalRenderer kHalfWidth
    static constexpr float kPortalHeightHalf       = 0.84375f;   // Portal-exact 1:1.6875 ratio (matches PortalRenderer kHalfHeight)

    // Tunables — Swirl (matches Portal's `portal_X_edge`). VALUES
    // EXTRACTED FROM `portals.pcf` (Portal 1 binary, decompiled with
    // srctools). Per-color emission rate, drag, twist sign mirror
    // portal_1_edge / portal_2_edge exactly. Per-particle alpha + color
    // are intentionally dim — the bright "energy ring" comes from
    // stacking 300+ low-alpha particles per second.
    //
    // Portal uses sprite-trail rendering (stretched billboards along
    // motion direction); we render as round billboards since our
    // particle shader doesn't support trails. The vortex motion is the
    // dominant visual feature so this approximation works.
    //
    // Units: Portal works in Hammer units (1u ≈ 1.905 cm). 50 units ≈
    // 0.95 m, matching our 1 m portal width.
    static constexpr float kSwirlSpawnRateBlue      = 300.0f; // per portal_1_edge
    static constexpr float kSwirlSpawnRateOrange    = 400.0f; // per portal_2_edge
    static constexpr float kSwirlBaseLifetime       = 1.0f;   // Portal default; alpha fades to 0 at lifetime end
    static constexpr float kSwirlLifetimeJitter     = 0.15f;
    static constexpr float kSwirlBirthSize          = 0.095f; // Portal radius=5 HU × radius_start_scale=2 = 10 HU = 0.19m peak (since size × scale=2 at birth)
    // Source-engine particle physics (authoritative formulas from
    // particles/builtin_particle_forces.cpp + builtin_particle_ops.cpp
    // in the 2017 engine leak — same code as Portal 1's particles.lib).
    //
    //   1 Hammer Unit = 0.01905 m.
    //   Particle mass = 1 (constant).
    //   "Pull towards control point": |F_HU/s²| = amount / len_HU^falloff_power.
    //     In our metres-based world: |F_m/s²| = amount × 0.01905² / r_m^falloff_power.
    //     For amount=1000, falloff=1: |F_m/s²| = 0.363 / r_m.
    //     (Not the constant 19.05 m/s² my previous implementation used!)
    //   "Twist around axis":          F = amount × cross(radial_unit, axis).
    //     Constant magnitude amount in HU/s² → amount × 0.01905 m/s².
    //   "Movement Basic" drag: velocity *= pow(1 - drag, dt * 30) per frame.
    //     drag is the fraction of velocity LOST per 1/30 s.
    //     For drag=0.3 at 60 fps: per-frame multiplier = pow(0.7, 0.5) ≈ 0.837.
    //     (NOT pow(drag, dt) — that's a totally different curve.)
    //
    // Net steady-state: swirl particles orbit at ~0.6 m/s (verified by
    // F_centripetal = F_pull → v² = 1000 HU²/s²; v ≈ 31.6 HU/s ≈ 0.6 m/s).
    // Trail length = speed × Trail Length Random (0.1-0.9 s) ≈ 0.06–0.54 m.
    static constexpr float kSwirlPullAmountBlue     = 1000.0f; // HU/s² at r=1 HU (Portal "Pull towards CP" amount)
    static constexpr float kSwirlTwistAmountBlue    = -300.0f; // HU/s² (Portal "twist around axis" amount; sign = CCW)
    static constexpr float kSwirlTwistAmountOrange  =  300.0f; // mirrored sign for orange
    static constexpr float kSwirlDragBlue           = 0.3f;    // PCF: drag=0.3 (loss per 1/30 s)
    static constexpr float kSwirlDragOrange         = 0.2f;    // PCF: drag=0.2
    // HU → m conversion used in the per-frame physics
    static constexpr float kHUToM                   = 0.01905f;

    // Tunables — Vacuum (Portal's `Portal_X_vacuum` child of
    // `portal_X_particles`, see portals_dump.txt:860-1131). Inward-pulled
    // cloud particles around the disc, giving the "this portal is sucking
    // in air" haze effect. Two emit_continuously operators sum 45/s.
    //   distance_min/max     = 100 HU = 1.905 m  (Position Within Sphere Random)
    //   warp                 = (0.6, 1, 1)        (Position Modify Warp Random)
    //   offset_normal        = 32 HU = 0.61 m     (Position Modify Offset Random — pushed forward along portal normal)
    //   lifetime             = 1.5 s              (Lifetime Random fixed)
    //   pull (constant)      = 400 HU/s² = 7.62 m/s²      (falloff=0)
    //   pull (1/r)           = 1000 HU/s² → 0.363/r_m m/s² (falloff=1)
    //   twist                = ±300 HU/s² = ±5.72 m/s²    (per-color sign)
    //   drag dampen          = 0.035 (Movement Dampen Relative to CP)
    //   radius_start_scale   = 0.5 → 0.1 over lifetime  (Radius Scale)
    //   color (blue)         = (4, 86, 252) sRGB        (Color Random fixed)
    //   sprite               = particle\smoke1\dust_motes (we reuse portal_X_particle as a tinted-soft-glow stand-in)
    static constexpr float kVacuumSpawnRate         = 45.0f;   // particles per second per portal
    static constexpr float kVacuumLifetime          = 1.5f;
    // PCF distances are calibrated for Portal's 32×54 HU portal half-
    // extents. Our portal is much smaller (0.5×0.84 m half-extents);
    // scaled to ~1.5× our half-width so the cloud sits ON / just
    // outside the disc rather than a full block away.
    static constexpr float kVacuumSpawnRadius_m     = 0.75f;          // ~1.5× our half-width
    static constexpr float kVacuumNormalOffset_m    = 0.25f;          // small forward push from the wall
    static constexpr float kVacuumWarpX             = 0.6f;            // PCF warp.x
    static constexpr float kVacuumPullConstant_mps2 = 400.0f * kHUToM; // = 7.62 m/s²
    static constexpr float kVacuumPullAmount_HU     = 1000.0f;         // 1/r force amount (uses kHUToM² / r_m)
    static constexpr float kVacuumTwistBlue_HU      = -300.0f;
    static constexpr float kVacuumTwistOrange_HU    =  300.0f;
    static constexpr float kVacuumDrag              = 0.035f;
    static constexpr float kVacuumBirthSize         = 0.10f;           // 5 HU radius × 0.5 start_scale; tuned for visibility
    static constexpr float kVacuumInitialOutward_mps = 80.0f * kHUToM; // PCF speed_in_local z=70-100 HU/s ≈ 1.5 m/s (midpoint)

    // Tunables — Close burst (Portal's `portal_X_close` PCF, see
    // portals_dump.txt:2215-2535). Vortex-pulled inward + tangential
    // twist + drag. Values converted from Hammer units to m/s²
    // (1 HU = 0.01905 m).
    //   num_to_emit          = 200  (instantaneous)
    //   lifetime             = 1.0  ± 0
    //   drag                 = 0.25
    //   pull (1st operator)  = 18000 HU/s² → 343 m/s²
    //   pull (delayed 0.6s)  = 500   HU/s² →   9.5 m/s²
    //   twist (blue)         = -150 HU/s²  →  -2.86 m/s²
    //   twist (orange)       = +150 HU/s²  →  +2.86 m/s² (mirrored sign)
    //   radius_start_scale   = 0.35 (shrinks to 0 over lifetime)
    //   min_distance         = 2 HU  → 0.038 m (constraint)
    //   max_distance         = 500 HU → 9.525 m (constraint)
    //   warp                 = 0.6, 1, 1 (blue squishes by 0.6 on right axis)
    // Close burst — same Source-leak formula conventions.
    //   pull (1st operator) amount=18000 HU/s², falloff=1 → |F_m/s²| = 18000 × 0.01905² / r_m = 6.53 / r_m
    //   pull (delayed)      amount=500   HU/s², falloff=0 (constant) → 0.01905 × 500 = 9.525 m/s² (additive after 0.6 s)
    //   twist               amount=±150 HU/s² → ±2.86 m/s² tangent (constant magnitude)
    static constexpr int   kCloseBurstCount       = 200;
    static constexpr float kCloseLifetime         = 1.0f;
    static constexpr float kCloseDrag             = 0.25f;
    static constexpr float kClosePullAmount       = 18000.0f; // HU/s² at r=1 HU, falloff=1
    static constexpr float kCloseLatePullForce    = 9.525f;   // m/s² (500 HU constant), enabled after 0.6 s
    static constexpr float kCloseLatePullDelaySec = 0.6f;
    static constexpr float kCloseTwistForceBlue   = -2.86f;   // m/s² (-150 HU)
    static constexpr float kCloseTwistForceOrange =  2.86f;   // m/s² (+150 HU)
    static constexpr float kCloseSpawnRadius      = 0.75f;    // m — spread spawn around portal disc
    static constexpr float kCloseInitialOutward   = 1.8f;     // m/s — small outward kick
    static constexpr float kCloseBirthSize        = 0.07f;    // m — radius_start_scale 0.35 × Portal radius 5

    // Tunables — BadSurface burst (no Portal equivalent — fits the
    // overall aesthetic). Short-lived sparks puffing outward.
    static constexpr int   kBadSurfaceCount       = 40;
    static constexpr float kBadSurfaceLifetime    = 0.5f;
    static constexpr float kBadSurfaceSpeed       = 2.5f;     // m/s outward
    static constexpr float kBadSurfaceBirthSize   = 0.06f;

    // Shader: simple unlit camera-facing billboard with quadratic alpha
    // falloff over lifetime. Vertex format = (pos3, uv2, color4ub) so it
    // reuses GetBlockVertexLayout() — same as the rest of the portal
    // pipeline.
    const char* PortalParticleSystem::s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;       // world-space position
layout(location = 1) in vec2 aUV;        // 0..1 across the billboard quad
layout(location = 2) in vec4 aColor;     // rgb=hot color, a=lifetime fraction (0=dead, 255=birth)

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

    const char* PortalParticleSystem::s_fragSource = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;

// Portal-extracted sprite. Per-batch binding selects blue
// (portal_1_particle.vtf, 64×64 soft glow) or orange
// (portal_2_particle.vtf, 128×128 swirly vortex). Falls back to
// procedural soft disc when uHasSprite=0.
uniform sampler2D uSprite;
uniform int       uHasSprite;

void main() {
    // vColor.a from the vertex is pre-computed on the C++ side as
    //   fadeMul × particle.alpha
    // where fadeMul is Source's SimpleSpline (Hermite smoothstep)
    // triangle profile: ramp-in over [0, 0.5] of life, ramp-out
    // over [0.5, 1.0] of life. See
    // builtin_particle_ops.cpp:271-402 (C_OP_FadeAndKill).
    float lifeAlpha = vColor.a;

    if (uHasSprite == 1) {
        // SPRITE PATH (Portal-extracted texture). Exact port of Source's
        // render_sprite_trail / render_animated_sprites formula:
        //
        //     output_rgb = sprite.rgb × particle.color × particle.alpha
        //
        // (Source SDK 2013, particles library — proprietary, but this
        // formula is documented in the renderer cpp + observable from
        // PCF authoring.) The sprite is sRGB-loaded (GPU converts on
        // sample), so sprite.rgb is already linear. The per-particle
        // color comes in via a unsigned-byte normalised vertex
        // attribute — vColor.rgb is the sRGB *byte* value scaled to
        // [0,1] but NOT sRGB-converted by the GPU (vertex attribs
        // aren't sRGB-aware). We do the sRGB→linear conversion here
        // (gamma 2.2 approximation) so Portal's authored colours like
        // (6, 99, 204) end up in linear space (≈0.0007, 0.124, 0.602)
        // rather than as-if-linear (0.024, 0.388, 0.800) — the latter
        // is 3-4× too bright and makes the additive blend stack to
        // white.
        vec4 s = texture(uSprite, vUV);
        float texLum = max(max(s.r, s.g), s.b);
        if (texLum < 0.005) discard;
        vec3 particleColorLinear = pow(vColor.rgb, vec3(2.2));
        FragColor = vec4(s.rgb * particleColorLinear * lifeAlpha, 1.0);
    } else {
        // PROCEDURAL FALLBACK — soft round disc tinted by per-particle
        // hot color. Used when sprite textures failed to load.
        float r = length(vUV - vec2(0.5)) * 2.0;
        float disc = 1.0 - smoothstep(0.55, 1.0, r);
        if (disc <= 0.0) discard;
        FragColor = vec4(vColor.rgb * disc * lifeAlpha, 1.0);
    }
}
)";

    PortalParticleSystem::PortalParticleSystem() = default;

    PortalParticleSystem::~PortalParticleSystem() {
        Shutdown();
    }

    void PortalParticleSystem::Shutdown() {
        if (!g_renderBackend) return;
        if (m_mesh != INVALID_MESH)            { g_renderBackend->DestroyMesh(m_mesh);              m_mesh = INVALID_MESH; }
        if (m_vb != INVALID_BUFFER)            { g_renderBackend->DestroyBuffer(m_vb);              m_vb = INVALID_BUFFER; }
        if (m_dummyTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_dummyTexture);   m_dummyTexture = INVALID_TEXTURE; }
        if (m_blueSprite != INVALID_TEXTURE)   { g_renderBackend->DestroyTexture(m_blueSprite);     m_blueSprite = INVALID_TEXTURE; }
        if (m_orangeSprite != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_orangeSprite);   m_orangeSprite = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)        { g_renderBackend->DestroyShader(m_shader);          m_shader = INVALID_SHADER; }
        m_particles.clear();
        m_spawnAccum.clear();
    }

    namespace {
        // Sprite textures are colour data (the actual rim spark + swirl
        // PNGs from Portal). Upload as sRGB so the GPU does the proper
        // sRGB → linear conversion on sample — without this they come
        // out ~2× too bright after we additively blend them.
        TextureHandle LoadPortalSprite(const char* path) {
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(path, &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[PortalParticleSystem] stbi_load failed for %s: %s",
                             path, stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(
                w, h, TextureFormat::SRGB8_A8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex,
                    TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(tex,
                    TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return tex;
        }
    } // namespace

    bool PortalParticleSystem::Initialize() {
        if (!g_renderBackend) return false;
        // Vulkan can't compile GLSL strings at runtime — load the pre-
        // compiled SPIR-V (shaders/portal_particle_vk.{vert,frag}.spv)
        // built by CMake's compile_shaders target. Same vertex format
        // (block layout, 24 bytes) and same push-constant uMVP, so no
        // RegisterShaderVertexLayout or portal-pipeline-layout calls
        // are needed — the default block layout path handles it.
        if (g_renderBackend->GetType() == BackendType::OpenGL) {
            m_shader = g_renderBackend->CreateShader(s_vertSource, s_fragSource);
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                "shaders/portal_particle.vert", "shaders/portal_particle.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[PortalParticleSystem] Failed to load shader for backend %s",
                         g_renderBackend->GetName());
            return false;
        }
        unsigned char white[] = {255, 255, 255, 255};
        m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);

        // Portal-extracted sprites (see assets/textures/portal/). Failure
        // is non-fatal — the shader falls back to procedural soft-disc
        // rendering via uHasSprite=0.
        m_blueSprite   = LoadPortalSprite("assets/textures/portal/portal_blue_particle.png");
        m_orangeSprite = LoadPortalSprite("assets/textures/portal/portal_orange_particle.png");
        if (m_blueSprite == INVALID_TEXTURE || m_orangeSprite == INVALID_TEXTURE) {
            Log::Warning("[PortalParticleSystem] One or both portal sprite textures "
                         "failed to load — falling back to procedural soft-disc rendering.");
        }

        // Allocate an initial-empty streaming VB. We grow it on demand.
        m_vbCapacityVerts = 1024;
        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
            m_vbCapacityVerts * 24, nullptr, BufferAccess::Streaming);
        m_mesh = g_renderBackend->CreateMesh(m_vb, INVALID_BUFFER, GetBlockVertexLayout());
        return true;
    }

    float& PortalParticleSystem::SpawnAccumFor(uint64_t gunId) {
        for (auto& [id, accum] : m_spawnAccum) {
            if (id == gunId) return accum;
        }
        m_spawnAccum.emplace_back(gunId, 0.0f);
        return m_spawnAccum.back().second;
    }

    namespace {
        // Cheap pseudo-random in [0, 1). Not cryptographic — fine for
        // particle jitter.
        float Frand() {
            return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        }

        // Per-pair palette lookup (matches PortalRenderer's kBluePalette /
        // kOrangePalette hot colors). Kept duplicated here rather than
        // shared via a header to keep PortalRenderer's anonymous-namespace
        // palette private to its TU.
        glm::vec3 BlueHot()   { return glm::vec3(130.0f/255.0f, 200.0f/255.0f, 255.0f/255.0f); }
        glm::vec3 OrangeHot() { return glm::vec3(255.0f/255.0f, 200.0f/255.0f, 130.0f/255.0f); }

        // Portal-exact swirl colors. Random lerp between color1 and color2
        // per particle (Portal "Color Random" operator, tint_perc=0).
        // BLUE: color1=(6,99,204), color2=(14,116,231)  — portal_1_edge
        // ORANGE: color1=(95,38,1), color2=(239,129,0)  — portal_2_edge
        glm::vec3 SwirlBlueColor(float t) {
            const glm::vec3 c1(6.0f/255.0f,  99.0f/255.0f, 204.0f/255.0f);
            const glm::vec3 c2(14.0f/255.0f, 116.0f/255.0f, 231.0f/255.0f);
            return glm::mix(c1, c2, t);
        }
        glm::vec3 SwirlOrangeColor(float t) {
            const glm::vec3 c1(95.0f/255.0f, 38.0f/255.0f,  1.0f/255.0f);
            const glm::vec3 c2(239.0f/255.0f, 129.0f/255.0f, 0.0f/255.0f);
            return glm::mix(c1, c2, t);
        }
        // Portal_X_vacuum Color Random — fixed colour (both stops equal,
        // tint_perc=0). Per PCF: blue = (4, 86, 252), orange = (255, 78, 0).
        glm::vec3 VacuumColor(bool isOrange) {
            return isOrange
                ? glm::vec3(255.0f/255.0f, 78.0f/255.0f, 0.0f/255.0f)
                : glm::vec3(4.0f/255.0f, 86.0f/255.0f, 252.0f/255.0f);
        }
    } // namespace

    void PortalParticleSystem::Update(float dt, const Client::ClientPortalManager& mgr) {
        if (m_shader == INVALID_SHADER) return;   // initialize failed

        // ── 1. Age existing particles, drop the dead ──
        for (auto& p : m_particles) {
            p.age += dt;
            p.position += p.velocity * dt;
            if (p.type == ParticleType::Projectile) {
                // Portal-gun projectile — constant velocity, no forces,
                // no drag. Just travels in a straight line at BLAST_SPEED
                // until its travel-time lifetime expires (then dies).
                // (Position already integrated via `p.position += p.velocity * dt` above.)
            } else if (p.type == ParticleType::Spark) {
                // Sparks: drag + warm-air drift.
                p.velocity *= std::pow(0.4f, dt);
                p.velocity.y += kVerticalDrift * dt;
            } else if (p.type == ParticleType::Vacuum) {
                // Vacuum cloud — Portal_X_vacuum physics. Pull is 3D
                // (Source's `Pull towards control point` operates on
                // the full r vector, not the in-plane projection), so
                // particles spawned in front of the portal get sucked
                // BACK TOWARD the portal centre — including along the
                // normal — instead of orbiting at constant depth.
                // Twist stays in-plane (cross-product around portal
                // normal as Source's `twist around axis` does).
                const glm::vec3 r3d = p.position - p.swirlOrigin;
                const float r3dLen = glm::length(r3d);
                if (r3dLen > 1.0e-4f) {
                    const glm::vec3 inward3d = -r3d / r3dLen;
                    // Constant-magnitude pull (falloff=0)
                    p.velocity += inward3d * kVacuumPullConstant_mps2 * dt;
                    // 1/r pull (falloff=1)
                    const float invRPull =
                        kVacuumPullAmount_HU * kHUToM * kHUToM / r3dLen;
                    p.velocity += inward3d * invRPull * dt;
                }
                // Twist — in-plane rotation around portal normal
                const glm::vec3 rPlane =
                    r3d - glm::dot(r3d, p.swirlNormal) * p.swirlNormal;
                const float rPlaneLen = glm::length(rPlane);
                if (rPlaneLen > 1.0e-4f) {
                    const glm::vec3 outwardPlane = rPlane / rPlaneLen;
                    const glm::vec3 tangentRaw =
                        glm::cross(outwardPlane, p.swirlNormal);
                    const float twistAmount = p.isOrange
                        ? kVacuumTwistOrange_HU
                        : kVacuumTwistBlue_HU;
                    p.velocity += tangentRaw * (twistAmount * kHUToM) * dt;
                }
                p.velocity *= std::pow(1.0f - kVacuumDrag, dt * 30.0f);
            } else if (p.type == ParticleType::CloseBurst) {
                // Close burst — same Source formulas as swirl, just
                // bigger amounts. Primary pull is falloff=1 (1/r);
                // delayed pull is falloff=0 (constant magnitude).
                const glm::vec3 r = p.position - p.swirlOrigin;
                const glm::vec3 rPlane =
                    r - glm::dot(r, p.swirlNormal) * p.swirlNormal;
                const float rLen = glm::length(rPlane);
                if (rLen > 0.038f) { // min_distance = 2 HU (per PCF)
                    const glm::vec3 outward    = rPlane / rLen;
                    const glm::vec3 inward     = -outward;
                    const glm::vec3 tangentRaw = glm::cross(outward, p.swirlNormal);
                    const float primaryAccel_mps2 =
                        kClosePullAmount * kHUToM * kHUToM / rLen;
                    p.velocity += inward * primaryAccel_mps2 * dt;
                    if (p.age >= kCloseLatePullDelaySec) {
                        p.velocity += inward * kCloseLatePullForce * dt;
                    }
                    // Signed twist — use the correct per-color constant.
                    const float twistAccel = p.isOrange
                        ? kCloseTwistForceOrange   // +2.86 m/s² (+150 HU)
                        : kCloseTwistForceBlue;    // −2.86 m/s² (−150 HU)
                    p.velocity += tangentRaw * twistAccel * dt;
                }
                p.velocity *= std::pow(1.0f - kCloseDrag, dt * 30.0f);
            } else {
                // Swirl: Source's "Pull towards control point" + "twist
                // around axis" + "Movement Basic" drag.
                const glm::vec3 r = p.position - p.swirlOrigin;
                const glm::vec3 rPlane =
                    r - glm::dot(r, p.swirlNormal) * p.swirlNormal;
                const float rLen = glm::length(rPlane);
                if (rLen > 1.0e-4f) {
                    const glm::vec3 outward = rPlane / rLen;
                    const glm::vec3 inward  = -outward;
                    const glm::vec3 tangentRaw =
                        glm::cross(outward, p.swirlNormal);
                    const float pullAccel_mps2 =
                        kSwirlPullAmountBlue * kHUToM * kHUToM / rLen;
                    p.velocity += inward * pullAccel_mps2 * dt;
                    const float twistAmountHU = p.isOrange
                        ? kSwirlTwistAmountOrange
                        : kSwirlTwistAmountBlue;
                    p.velocity += tangentRaw * (twistAmountHU * kHUToM) * dt;
                }
                const float dragFrac = p.isOrange ? kSwirlDragOrange : kSwirlDragBlue;
                p.velocity *= std::pow(1.0f - dragFrac, dt * 30.0f);
            }
        }
        m_particles.erase(
            std::remove_if(m_particles.begin(), m_particles.end(),
                [](const Particle& p) { return p.age >= p.lifetime; }),
            m_particles.end());
        m_particles.erase(
            std::remove_if(m_particles.begin(), m_particles.end(),
                [](const Particle& p) { return p.age >= p.lifetime; }),
            m_particles.end());

        // ── 2. Spawn around active portals ──
        // Walks every pair, both colours; spawns at the rim with random
        // angular position. Spawn rate is per-portal (so an active pair
        // emits twice as many as one orphan portal).
        auto spawnFromPortal = [&](const Client::ClientPortal& portal,
                                   uint64_t gunId, bool isOrange) {
            if (!portal.active) return;
            // Per-portal spawn-accumulator key uses (gunId, color) packed
            // so blue/orange of the same gun emit independently.
            const uint64_t key = (gunId << 1) | (isOrange ? 1ULL : 0ULL);
            float& accum = SpawnAccumFor(key);
            accum += dt * kSpawnRatePerPortal;
            int spawnCount = static_cast<int>(accum);
            accum -= static_cast<float>(spawnCount);

            for (int i = 0; i < spawnCount; ++i) {
                // Random angle around the rim. Convert to portal-local
                // (right, up) offset on the 1×2 ellipse.
                float angle = Frand() * 6.2831853f;
                float lx = std::cos(angle) * kPortalWidthHalf;
                float ly = std::sin(angle) * kPortalHeightHalf;

                glm::vec3 worldPos = glm::vec3(portal.origin)
                    + portal.right * lx
                    + portal.upDir * ly;

                // Outward radial direction in world (along portal's right/up).
                glm::vec3 outward = glm::normalize(
                    portal.right * std::cos(angle)
                    + portal.upDir * std::sin(angle));

                // Tangential jitter direction (perpendicular to outward,
                // in the portal's plane).
                glm::vec3 tangent = glm::cross(glm::vec3(portal.normal), outward);

                Particle p;
                p.position = worldPos;
                p.velocity = outward * kRadialSpeed
                           + tangent * (Frand() - 0.5f) * 2.0f * kTangentialSpeedJitter;
                // Portal's portal_X_particles defines NO Color Random
                // operator (verified in portals_dump.txt:707-855) — the
                // per-particle colour defaults to white. The sprite
                // texture itself carries the blue/orange hue.
                p.colorHot = glm::vec3(1.0f, 1.0f, 1.0f);
                p.age = 0.0f;
                p.lifetime = kBaseLifetime + (Frand() - 0.5f) * 2.0f * kLifetimeJitter;
                p.birthSizeM = kBirthSize;
                // Portal's portal_X_particles Alpha Random is fixed at
                // alpha_min=32, alpha_max=32 → exact constant 32/255.
                p.alphaMul = 32.0f / 255.0f;
                p.isOrange = isOrange;
                m_particles.push_back(p);
            }
        };

        // Swirl emitter — Portal's `portal_X_edge` (vortex around rim).
        // Spawn parameters mirror the PCF: random angular position on
        // an oval rim (warped by kSwirlWarpX along the portal right
        // axis), small initial tangential kick, low per-particle alpha
        // (the bright ring comes from stacking 300+ particles).
        // Per-frame forces (pull toward center + twist around normal +
        // drag) live in the physics loop above.
        auto spawnSwirlFromPortal = [&](const Client::ClientPortal& portal,
                                        uint64_t gunId, bool isOrange) {
            if (!portal.active) return;
            const float rate = isOrange ? kSwirlSpawnRateOrange
                                        : kSwirlSpawnRateBlue;
            const uint64_t key = ((gunId << 1) | (isOrange ? 1ULL : 0ULL))
                               | (1ULL << 62);
            float& accum = SpawnAccumFor(key);
            accum += dt * rate;
            int spawnCount = static_cast<int>(accum);
            accum -= static_cast<float>(spawnCount);

            // (Twist sign is applied per-frame in the physics loop
            // using p.isOrange — see Update().)

            for (int i = 0; i < spawnCount; ++i) {
                // Spawn position — Portal's PCF spawn radius (50 HU) is
                // calibrated for Portal's portal size (32×54 HU
                // half-extents). Our portal is smaller (0.5×0.84375 m
                // half-extents), so we use Portal's SPAWN-TO-RIM RATIOS
                // rather than the raw HU values:
                //
                //   Portal spawn X extent = 32.5 HU = 1.016 × half-width
                //   Portal spawn Y extent = 50.0 HU = 0.926 × half-height
                //
                // (Spawn Y is slightly INSIDE the rim, spawn X is slightly
                // OUTSIDE — together they form the warped ellipse on/near
                // the visible rim where Portal's swirl particles live.)
                // Bias.z = 0.07 gives a thin normal-axis fuzz.
                constexpr float kPortalHalfWidth_m  = 0.5f;        // matches PortalRenderer kHalfWidth
                constexpr float kPortalHalfHeight_m = 0.84375f;    // matches PortalRenderer kHalfHeight
                constexpr float kSpawnXFactor       = 1.016f;      // PCF: 32.5/32 HU
                constexpr float kSpawnYFactor       = 0.926f;      // PCF: 50/54 HU
                constexpr float kSpawnNormalBias    = 0.07f;       // PCF distance_bias.z

                const float angle = Frand() * 6.2831853f;
                const float cosA = std::cos(angle);
                const float sinA = std::sin(angle);
                // Spawn directly on the warped rim ellipse (matches
                // Portal's spawn-to-rim relationship exactly).
                const float lx = cosA * kPortalHalfWidth_m  * kSpawnXFactor;
                const float ly = sinA * kPortalHalfHeight_m * kSpawnYFactor;
                // Tiny normal-axis jitter for ring thickness.
                const float spawnRadiusAvg_m =
                    0.5f * (kPortalHalfWidth_m * kSpawnXFactor
                          + kPortalHalfHeight_m * kSpawnYFactor);
                const float ln = (Frand() * 2.0f - 1.0f) * spawnRadiusAvg_m * kSpawnNormalBias;

                const glm::vec3 worldPos = glm::vec3(portal.origin)
                    + portal.right * lx
                    + portal.upDir * ly
                    + portal.normal * ln;

                Particle p;
                p.position = worldPos;
                // PCF: speed_min=speed_max=0 → spawn AT REST. Twist + pull
                // forces in the per-frame physics accelerate them to
                // orbital velocity over the first ~50 ms of life.
                p.velocity = glm::vec3(0.0f);
                // Random lerp between Portal's two color stops.
                const float t = Frand();
                p.colorHot = isOrange ? SwirlOrangeColor(t)
                                      : SwirlBlueColor(t);
                p.age = 0.0f;
                p.lifetime = kSwirlBaseLifetime
                           + (Frand() - 0.5f) * 2.0f * kSwirlLifetimeJitter;
                p.birthSizeM = kSwirlBirthSize;
                p.type = ParticleType::Swirl;
                p.swirlOrigin = glm::vec3(portal.origin);
                p.swirlNormal = portal.normal;
                // Portal Alpha Random 5-50/255 — very low per-particle.
                p.alphaMul = (5.0f + Frand() * 45.0f) / 255.0f;
                // Portal Trail Length Random: 0.1-0.9 blue, 0.1-0.75 orange.
                const float trailMax = isOrange ? 0.75f : 0.9f;
                p.trailSec = 0.1f + Frand() * (trailMax - 0.1f);
                p.isOrange = isOrange;
                // PCF Rotation Random: rotation_offset_min=0, max=360°.
                p.rotation = Frand() * 6.2831853f;
                m_particles.push_back(p);
            }
        };

        // Vacuum spawn — Portal's `Portal_X_vacuum` child of
        // `portal_X_particles`. Inward-pulled cloud of soft particles
        // around the disc. Spawn at ~100 HU radius (warped 0.6 on X)
        // pushed 32 HU forward along portal normal, with small outward
        // initial velocity. Pull + twist forces in the per-frame physics
        // suck them into the portal centre over ~1.5 s lifetime.
        auto spawnVacuumFromPortal = [&](const Client::ClientPortal& portal,
                                          uint64_t gunId, bool isOrange) {
            if (!portal.active) return;
            const uint64_t key = ((gunId << 1) | (isOrange ? 1ULL : 0ULL))
                               | (2ULL << 62);  // separate accumulator bucket
            float& accum = SpawnAccumFor(key);
            accum += dt * kVacuumSpawnRate;
            int spawnCount = static_cast<int>(accum);
            accum -= static_cast<float>(spawnCount);

            for (int i = 0; i < spawnCount; ++i) {
                const float angle = Frand() * 6.2831853f;
                const float cosA = std::cos(angle);
                const float sinA = std::sin(angle);
                // In-plane radial offset (warped 0.6× along X)
                const float lx = cosA * kVacuumSpawnRadius_m * kVacuumWarpX;
                const float ly = sinA * kVacuumSpawnRadius_m;
                // Pushed forward along portal normal
                const glm::vec3 worldPos = glm::vec3(portal.origin)
                    + portal.right  * lx
                    + portal.upDir  * ly
                    + portal.normal * kVacuumNormalOffset_m;

                Particle p;
                p.position    = worldPos;
                // Small outward initial speed along portal normal (PCF
                // speed_in_local z=70-100 HU/s).
                p.velocity    = portal.normal * kVacuumInitialOutward_mps;
                p.colorHot    = VacuumColor(isOrange);
                p.age         = 0.0f;
                p.lifetime    = kVacuumLifetime;
                p.birthSizeM  = kVacuumBirthSize;
                p.type        = ParticleType::Vacuum;
                p.swirlOrigin = glm::vec3(portal.origin);
                p.swirlNormal = portal.normal;
                p.alphaMul    = 0.6f;   // moderate per-particle alpha — PCF uses
                                        // Alpha Fade In/Out Random to ramp; we
                                        // skip the operator chain and rely on
                                        // the SimpleSpline triangle fade in
                                        // Render() to handle the fade-in/out.
                p.trailSec    = 0.0f;   // round billboards (not sprite trails)
                p.isOrange    = isOrange;
                p.rotation    = Frand() * 6.2831853f;
                m_particles.push_back(p);
            }
        };

        mgr.ForEachPair([&](uint64_t gunId, const Client::ClientPortalPair& pair) {
            spawnFromPortal(pair.blue,   gunId, /*isOrange=*/false);
            spawnFromPortal(pair.orange, gunId, /*isOrange=*/true);
            spawnSwirlFromPortal(pair.blue,   gunId, /*isOrange=*/false);
            spawnSwirlFromPortal(pair.orange, gunId, /*isOrange=*/true);
            spawnVacuumFromPortal(pair.blue,   gunId, /*isOrange=*/false);
            spawnVacuumFromPortal(pair.orange, gunId, /*isOrange=*/true);
        });
    }

    void PortalParticleSystem::EmitProjectile(const glm::vec3& start,
                                              const glm::vec3& end,
                                              bool isOrange) {
        if (m_shader == INVALID_SHADER) return;

        // Portal exact constants from weapon_portalgun.cpp.
        constexpr float kBlastSpeed_HU_per_s = 3000.0f;   // weapon_portalgun.cpp:28
        constexpr float kBlastSpeed_m_per_s  = kBlastSpeed_HU_per_s * 0.01905f; // 57.15 m/s
        constexpr float kMaxProjectileDelay  = 0.5f;      // weapon_portalgun.cpp:82 sv_portal_projectile_delay

        glm::vec3 dir = end - start;
        float dist = glm::length(dir);
        if (dist < 1.0e-3f) return;   // gun is already at the impact point — skip
        dir /= dist;

        // We don't have a gun viewmodel, so the projectile would
        // otherwise spawn AT the camera eye — distance 0 from the
        // camera, clipped by the near plane on frame 1, then a
        // dead-center tiny dot for the rest of its flight. Offset
        // the spawn 0.5 m forward along the aim direction so it's
        // visible from frame 1 (and reduce the remaining travel
        // distance correspondingly).
        constexpr float kSpawnForwardOffset = 0.5f;
        glm::vec3 spawnPos = start + dir * kSpawnForwardOffset;
        dist -= kSpawnForwardOffset;
        if (dist < 1.0e-3f) return;   // impact within 0.5 m of camera — skip

        // Travel time = remaining distance / speed, capped at the Portal max delay.
        const float travelTime = std::min(dist / kBlastSpeed_m_per_s,
                                          kMaxProjectileDelay);

        Particle p;
        p.position    = spawnPos;
        p.velocity    = dir * kBlastSpeed_m_per_s;
        p.colorHot    = isOrange ? OrangeHot() : BlueHot();
        p.age         = 0.0f;
        p.lifetime    = travelTime;
        p.birthSizeM  = 0.30f;         // bigger so it's visible in first-person
        p.type        = ParticleType::Projectile;
        p.alphaMul    = 1.0f;          // full brightness — solid energy bolt
        p.trailSec    = 0.0f;          // ROUND camera-facing billboard
                                       // (a trail-stretched quad foreshortens
                                       // to a line when viewed head-on, which
                                       // is exactly the first-person POV when
                                       // you fire; round billboard stays a
                                       // visible bright dot regardless).
        p.isOrange    = isOrange;
        p.rotation    = 0.0f;
        m_particles.push_back(p);
    }

    void PortalParticleSystem::EmitOneShot(BurstKind kind,
                                           const glm::vec3& origin,
                                           const glm::vec3& normal,
                                           bool isOrange) {
        if (m_shader == INVALID_SHADER) return; // init failed; silently skip

        // Build an orthonormal basis (right, up) in the plane perpendicular
        // to `normal` so we can spawn particles around the portal disc.
        glm::vec3 nrm = glm::length(normal) > 1e-4f
            ? glm::normalize(normal)
            : glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(nrm, worldUp)) > 0.99f) {
            // Floor / ceiling portal — use world +X as the seed instead.
            worldUp = glm::vec3(1.0f, 0.0f, 0.0f);
        }
        const glm::vec3 right = glm::normalize(glm::cross(worldUp, nrm));
        const glm::vec3 up    = glm::normalize(glm::cross(nrm, right));

        if (kind == BurstKind::Close) {
            // Close burst — 200 particles spawned on/just outside the
            // portal disc, vortex-pulled inward over 1s. Twist sign
            // mirrors per-color rotation (orange twists +, blue twists -).
            const float twist = isOrange ? kCloseTwistForceOrange
                                         : kCloseTwistForceBlue;
            const float spinSign = (twist > 0.0f) ? 1.0f : -1.0f;
            for (int i = 0; i < kCloseBurstCount; ++i) {
                const float angle = Frand() * 6.2831853f;
                // Random radius within kCloseSpawnRadius — Portal's
                // Position Modify Warp Random spawns within a box; we
                // approximate with a disc of equivalent radius.
                const float r = kCloseSpawnRadius * std::sqrt(Frand());
                // Apply Portal's warp=(0.6, 1, 1): squish right axis.
                const float lx = std::cos(angle) * r * 0.6f;
                const float ly = std::sin(angle) * r;

                const glm::vec3 spawnPos = origin + right * lx + up * ly;
                const glm::vec3 outwardInPlane = glm::length(
                    right * std::cos(angle) + up * std::sin(angle)) > 1e-4f
                    ? glm::normalize(right * std::cos(angle) + up * std::sin(angle))
                    : right;
                const glm::vec3 tangent = glm::cross(nrm, outwardInPlane);

                Particle p;
                p.position = spawnPos;
                // Small outward kick + tangent spin so the pull-inward
                // motion has visible curl from the start.
                p.velocity = outwardInPlane * kCloseInitialOutward
                           + tangent * (kCloseInitialOutward * spinSign * 0.5f);
                p.colorHot = isOrange ? SwirlOrangeColor(Frand())
                                      : SwirlBlueColor(Frand());
                p.age      = 0.0f;
                p.lifetime = kCloseLifetime;
                p.birthSizeM = kCloseBirthSize;
                p.type     = ParticleType::CloseBurst;
                p.swirlOrigin = origin;
                p.swirlNormal = nrm;
                // Portal Alpha Random 220-255 → 0.86-1.0 here.
                p.alphaMul = (220.0f + Frand() * 35.0f) / 255.0f;
                // Trail length 0-8 HU → 0-0.15 m; Portal renders close
                // burst as a sprite trail (min length=0, max length=8).
                p.trailSec = 0.05f + Frand() * 0.20f;
                p.isOrange = isOrange;
                p.rotation = Frand() * 6.2831853f;
                m_particles.push_back(p);
            }

            // ── Close-flash child effect (PCF: portal_X_close has
            // child "portal_X_close_flash") ──
            // Source PCF describes this as a short, bright additional
            // burst on top of the main close burst — a quick "pop"
            // before the main vortex collapses. Implementation: 50
            // extra spark-style particles with very short lifetime,
            // brighter alpha, spawning right at the origin and
            // bursting outward in the +normal hemisphere.
            constexpr int   kFlashCount     = 50;
            constexpr float kFlashLifetime  = 0.30f;
            constexpr float kFlashSpeedMin  = 2.5f;
            constexpr float kFlashSpeedMax  = 5.0f;
            constexpr float kFlashBirthSize = 0.08f;
            const glm::vec3 flashHot = isOrange ? OrangeHot() : BlueHot();
            for (int i = 0; i < kFlashCount; ++i) {
                const float az  = Frand() * 6.2831853f;
                const float pol = std::acos(0.3f + Frand() * 0.7f);
                const glm::vec3 dir = std::sin(pol) * std::cos(az) * right
                                    + std::sin(pol) * std::sin(az) * up
                                    + std::cos(pol) * nrm;
                Particle pf;
                pf.position   = origin;
                pf.velocity   = dir * (kFlashSpeedMin + Frand() * (kFlashSpeedMax - kFlashSpeedMin));
                pf.colorHot   = flashHot;
                pf.age        = 0.0f;
                pf.lifetime   = kFlashLifetime + Frand() * 0.08f;
                pf.birthSizeM = kFlashBirthSize;
                pf.type       = ParticleType::Spark;
                pf.alphaMul   = 0.9f;   // bright flash (vs swirl's 0.02-0.2)
                pf.trailSec   = 0.0f;
                pf.isOrange   = isOrange;
                pf.rotation   = Frand() * 6.2831853f;
                m_particles.push_back(pf);
            }
        } else { // BurstKind::BadSurface
            // BadSurface — quick puff of sparks expanding outward from
            // the hit point. Re-uses Spark physics (drag + warm-air
            // drift) so they fade naturally.
            const glm::vec3 baseHot = isOrange ? OrangeHot() : BlueHot();
            for (int i = 0; i < kBadSurfaceCount; ++i) {
                // Random direction biased toward the +normal hemisphere
                // (we don't want sparks shooting INTO the wall).
                const float az  = Frand() * 6.2831853f;
                const float pol = std::acos(0.2f + Frand() * 0.8f); // upper cap
                const glm::vec3 dir = std::sin(pol) * std::cos(az) * right
                                    + std::sin(pol) * std::sin(az) * up
                                    + std::cos(pol) * nrm;
                Particle p;
                p.position = origin;
                p.velocity = dir * (kBadSurfaceSpeed * (0.5f + Frand()));
                p.colorHot = baseHot;
                p.age      = 0.0f;
                p.lifetime = kBadSurfaceLifetime + Frand() * 0.15f;
                p.birthSizeM = kBadSurfaceBirthSize;
                p.type     = ParticleType::Spark;
                p.alphaMul = 1.0f;
                p.trailSec = 0.0f;
                p.isOrange = isOrange;
                m_particles.push_back(p);
            }
        }
    }

    void PortalParticleSystem::Render(const glm::mat4& projection,
                                      const glm::mat4& view,
                                      const glm::vec3& cameraPos) {
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        if (m_particles.empty()) return;

        // Build the VB on the CPU — one camera-facing quad per particle.
        // Per-frame upload of N×6 verts (tiny — N ≤ ~50 typical).
        struct Vert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        // Two batches — blue first, orange after. Each batch is drawn
        // with the matching Portal-extracted sprite bound. The blue
        // batch's starting offset is always 0; the orange batch starts
        // at blueVertCount.
        std::vector<Vert> verts;
        verts.reserve(m_particles.size() * 6);

        // Two-pass write: first all blue particles, then all orange.
        // Two passes over m_particles is cheap (N ≤ ~500 typical) and
        // avoids a sort allocation. Track each batch's vertex count.
        uint32_t blueVerts = 0, orangeVerts = 0;

        // Camera basis for billboarding — derive right & up from the view
        // matrix's first two rows (which are world right and world up
        // respectively when the view matrix is built from lookAt).
        const glm::vec3 camRight (view[0][0], view[1][0], view[2][0]);
        const glm::vec3 camUp    (view[0][1], view[1][1], view[2][1]);

        auto emitOne = [&](const Particle& p) {
            const float lifeFrac = 1.0f - (p.age / p.lifetime);   // 1→0
            // Particles SHRINK as they fade. Sparks shrink modestly;
            // Swirl shrinks 2x→0 per Portal "Radius Scale" operator
            // (radius_start_scale=2, radius_end_scale=0); CloseBurst
            // shrinks 1→0 per portal_X_close's radius_start_scale=0.35
            // (the 0.35 itself is baked into kCloseBirthSize).
            float sizeMul;
            switch (p.type) {
                case ParticleType::Swirl:      sizeMul = lifeFrac * 2.0f;            break;
                case ParticleType::CloseBurst: sizeMul = lifeFrac;                   break;
                case ParticleType::Vacuum:
                    // PCF: radius_start_scale=0.5 → radius_end_scale=0.1
                    // (linear blend over lifetime).
                    sizeMul = 0.5f * lifeFrac + 0.1f * (1.0f - lifeFrac);
                    break;
                case ParticleType::Projectile:
                    // Constant size — energy bolt stays the same scale
                    // for its whole travel.
                    sizeMul = 1.0f;
                    break;
                default:                       sizeMul = 0.45f + 0.55f * lifeFrac;   break;
            }
            const float size = p.birthSizeM * sizeMul;

            // ── Source's "Alpha Fade and Decay" (C_OP_FadeAndKill) ──
            // Per-particle-type PCF parameters (from portals_dump.txt):
            //   Swirl (portal_X_edge):  fade_in [0, 0.5], HOLD [0.5, 0.75],
            //                           fade_out [0.75, 1.0].
            //   CloseBurst (portal_X_close): fade_in [0, 0.5], HOLD [0.5, 0.5],
            //                                fade_out [0.5, 1.0] (symmetric triangle).
            //   Spark:  fade_in [0, 0.5], fade_out [0.5, 1.0].
            // SimpleSpline (Hermite smoothstep) at each transition.
            const float age01 = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
            float fadeIn0, fadeIn1, fadeOut0, fadeOut1;
            switch (p.type) {
                case ParticleType::Swirl:
                    fadeIn0  = 0.0f;  fadeIn1  = 0.5f;
                    fadeOut0 = 0.75f; fadeOut1 = 1.0f;
                    break;
                case ParticleType::Vacuum:
                    // Portal_X_vacuum PCF: Alpha Fade In Random (0.25 s)
                    // + Alpha Fade Out Random (0.125 s) over 1.5 s life
                    // → fadeIn fraction = 0.25/1.5 ≈ 0.167,
                    //   fadeOut fraction = 0.125/1.5 ≈ 0.083.
                    fadeIn0  = 0.0f;  fadeIn1  = 0.167f;
                    fadeOut0 = 0.917f; fadeOut1 = 1.0f;
                    break;
                case ParticleType::Projectile:
                    // Energy bolt — fade in quickly, hold bright the whole
                    // travel, brief snap-off at impact (avoids div-by-zero
                    // in the SimpleSpline path below; just makes the last
                    // 1% of life fade out).
                    fadeIn0  = 0.0f;  fadeIn1  = 0.1f;
                    fadeOut0 = 0.99f; fadeOut1 = 1.0f;
                    break;
                default: // Spark + CloseBurst
                    fadeIn0  = 0.0f; fadeIn1  = 0.5f;
                    fadeOut0 = 0.5f; fadeOut1 = 1.0f;
                    break;
            }
            float fadeMul;
            if (age01 < fadeIn1) {
                const float t = (age01 - fadeIn0) / (fadeIn1 - fadeIn0);
                fadeMul = t * t * (3.0f - 2.0f * t);  // SimpleSpline 0→1
            } else if (age01 < fadeOut0) {
                fadeMul = 1.0f;                        // HOLD at peak
            } else {
                const float t = (age01 - fadeOut0) / (fadeOut1 - fadeOut0);
                fadeMul = 1.0f - t * t * (3.0f - 2.0f * t);  // SimpleSpline 1→0
            }
            const uint8_t alphaByte = static_cast<uint8_t>(
                std::clamp(fadeMul * p.alphaMul, 0.0f, 1.0f) * 255.0f);
            const uint8_t r = static_cast<uint8_t>(std::clamp(p.colorHot.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t g = static_cast<uint8_t>(std::clamp(p.colorHot.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t b = static_cast<uint8_t>(std::clamp(p.colorHot.b, 0.0f, 1.0f) * 255.0f);

            // Compute the quad axes. For trail particles (trailSec > 0)
            // the long axis follows the velocity direction in world
            // space and stretches backwards by trailSec × speed; the
            // short axis is perpendicular, screen-aligned. For round
            // particles the quad is camera-axis-aligned.
            glm::vec3 longAxis;
            glm::vec3 shortAxis;
            glm::vec3 frontCenter = p.position;
            glm::vec3 backCenter  = p.position;
            const float speed = glm::length(p.velocity);
            if (p.trailSec > 0.0f && speed > 0.05f) {
                const glm::vec3 dir = p.velocity / speed;
                // Source's render_sprite_trail formula (verified in
                // builtin_particle_render_ops.cpp:1350-1620):
                //   length = speed × trail_length_attribute (seconds)
                //   length = clamp(length, min_length, max_length)
                // PCF for portal_X_edge: min_length=0, max_length=2000 HU.
                // 2000 HU × 0.01905 m/HU = 38.1 m max trail.
                constexpr float kMinLengthM = 0.0f;
                constexpr float kMaxLengthM = 38.1f;
                const float trailLen = std::clamp(
                    speed * p.trailSec, kMinLengthM, kMaxLengthM);
                backCenter = p.position - dir * trailLen;
                // Short axis = perpendicular to dir in screen-aligned
                // plane (cross with view direction at particle).
                const glm::vec3 view = glm::normalize(p.position - cameraPos);
                glm::vec3 perp = glm::cross(dir, view);
                const float perpLen = glm::length(perp);
                if (perpLen > 1.0e-4f) {
                    shortAxis = (perp / perpLen) * size;
                } else {
                    shortAxis = camRight * size; // degenerate (view parallel to dir)
                }
                longAxis = dir * 0.0f; // not used; we use front/backCenter directly
            } else {
                // Round billboard, camera-axis aligned. Apply Source's
                // "Rotation Random" by rotating the camera basis around
                // the camera-forward axis by p.rotation radians (matches
                // particles rendering in screen-aligned 2D).
                const float c = std::cos(p.rotation);
                const float s = std::sin(p.rotation);
                shortAxis = ( camRight * c + camUp    * s) * size;
                longAxis  = (-camRight * s + camUp    * c) * size;
                frontCenter = p.position + longAxis;
                backCenter  = p.position - longAxis;
            }

            const glm::vec3 c0 = frontCenter - shortAxis;
            const glm::vec3 c1 = frontCenter + shortAxis;
            const glm::vec3 c2 = backCenter  + shortAxis;
            const glm::vec3 c3 = backCenter  - shortAxis;

            verts.push_back({c0.x, c0.y, c0.z, 0.0f, 0.0f, r, g, b, alphaByte});
            verts.push_back({c1.x, c1.y, c1.z, 1.0f, 0.0f, r, g, b, alphaByte});
            verts.push_back({c2.x, c2.y, c2.z, 1.0f, 1.0f, r, g, b, alphaByte});
            verts.push_back({c0.x, c0.y, c0.z, 0.0f, 0.0f, r, g, b, alphaByte});
            verts.push_back({c2.x, c2.y, c2.z, 1.0f, 1.0f, r, g, b, alphaByte});
            verts.push_back({c3.x, c3.y, c3.z, 0.0f, 1.0f, r, g, b, alphaByte});
        };

        // Two passes — blue first so the orange batch's draw offset is
        // simply blueVerts. This lets the orange particles bind their
        // own sprite without re-sorting the VB.
        for (const auto& p : m_particles) {
            if (!p.isOrange) { emitOne(p); blueVerts += 6; }
        }
        for (const auto& p : m_particles) {
            if (p.isOrange)  { emitOne(p); orangeVerts += 6; }
        }

        if (verts.empty()) return;

        // Grow the VB if needed. Power-of-two growth so we don't realloc
        // every few particles.
        if (verts.size() > m_vbCapacityVerts) {
            size_t newCap = m_vbCapacityVerts;
            while (newCap < verts.size()) newCap *= 2;
            g_renderBackend->DestroyBuffer(m_vb);
            m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                newCap * 24, nullptr, BufferAccess::Streaming);
            // Re-register the mesh against the new VB.
            g_renderBackend->DestroyMesh(m_mesh);
            m_mesh = g_renderBackend->CreateMesh(m_vb, INVALID_BUFFER, GetBlockVertexLayout());
            m_vbCapacityVerts = newCap;
        }
        g_renderBackend->UpdateBuffer(m_vb, 0, verts.size() * 24, verts.data());

        // Additive blending — particles add light without darkening
        // anything behind. Depth-test on (so they hide behind walls /
        // players in front of the portal), depth-write off (so they
        // don't occlude each other in weird order-dependent ways).
        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = false;
        s.colorWriteEnabled = true;
        s.blendEnabled      = true;
        s.srcBlendFactor    = BlendFactor::One;
        s.dstBlendFactor    = BlendFactor::One;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);

        g_renderBackend->BindShader(m_shader);
        const glm::mat4 mvp = projection * view;
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);

        // Issue one DrawArrays per colour batch using `firstVertex` to
        // split them (blue occupies [0, blueVerts), orange occupies
        // [blueVerts, blueVerts+orangeVerts)). The full upload happened
        // above (single UpdateBuffer with all verts).
        //
        // We can't double-buffer at offset 0 like the original GL code
        // did: Vulkan's UpdateBuffer maps + memcpys immediately at
        // command-recording time, but the recorded vkCmdDraws don't
        // execute until vkQueueSubmit at end-of-frame. If we memcpy
        // blue→offset 0, record draw, memcpy orange→offset 0, record
        // draw, then at GPU-exec time BOTH draws see the second memcpy
        // (orange data). Visible symptom on Vulkan: "no particles for
        // an activated blue portal" — the blue draw renders orange
        // vertex positions, so nothing appears at the blue portal,
        // while the orange draw works correctly.
        if (blueVerts > 0) {
            g_renderBackend->BindTexture(
                m_blueSprite != INVALID_TEXTURE ? m_blueSprite : m_dummyTexture, 0);
            g_renderBackend->SetUniformInt(m_shader, "uSprite", 0);
            g_renderBackend->SetUniformInt(m_shader, "uHasSprite",
                m_blueSprite != INVALID_TEXTURE ? 1 : 0);
            g_renderBackend->DrawArrays(m_mesh, blueVerts, /*firstVertex=*/0);
        }
        if (orangeVerts > 0) {
            g_renderBackend->BindTexture(
                m_orangeSprite != INVALID_TEXTURE ? m_orangeSprite : m_dummyTexture, 0);
            g_renderBackend->SetUniformInt(m_shader, "uSprite", 0);
            g_renderBackend->SetUniformInt(m_shader, "uHasSprite",
                m_orangeSprite != INVALID_TEXTURE ? 1 : 0);
            g_renderBackend->DrawArrays(m_mesh, orangeVerts, /*firstVertex=*/blueVerts);
        }
        g_renderBackend->UnbindMesh();

        // Restore default pipeline (no blending).
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);

        // Suppress unused-parameter warning on cameraPos (kept in the API
        // for forward-compat: distance-based culling, LOD, etc.).
        (void)cameraPos;
    }

} // namespace Render

#endif // ENABLE_PORTAL_GUN
