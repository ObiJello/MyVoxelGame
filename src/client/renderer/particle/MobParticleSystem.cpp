// File: src/client/renderer/particle/MobParticleSystem.cpp
// See header for scope and the list of deliberate deviations.

#include "MobParticleSystem.hpp"
#include "common/core/Features.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../environment/EntityEnvironment.hpp"
#include "common/world/lighting/LightCoords.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/PotentSulfurBlock.hpp"
#include "common/world/fluid/FluidState.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#endif
#include "platform/GameDirectory.hpp"

#include "stb_image.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    MobParticleSystem g_mobParticleSystem;

    // Shader — camera-facing textured billboard, texture × per-particle
    // colour, alpha blend with MC's 0.1 cutout so hard sprite edges stay
    // crisp against depth-tested geometry. Vertex format is the standard
    // 24-byte block layout so the default mesh path handles it on both
    // backends (same trick as PortalParticleSystem).
    const char* MobParticleSystem::s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;    // render-space corner
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;  // particle rCol/gCol/bCol/alpha

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;
out vec3 vRenderPos;   // for the fog

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vRenderPos = aPos;
}
)";

    const char* MobParticleSystem::s_fragSource = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec3 vRenderPos;
out vec4 FragColor;

uniform sampler2D uSprite;
// MC particle.fsh: the lightmap, then the fog — the terrain's fog, so a
// particle fades into the Blindness black with everything else.
uniform vec3 uEntityLight;    // the draw's lightmap colour (EntityEnvironment.hpp)
uniform vec3  uCameraPos;     // render space
uniform vec4  uFogColor;      // rgb, a = strength
uniform vec4  uFogEnv;        // (envStart, envEnd, rdStart, rdEnd)

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 s = texture(uSprite, vUV);
    vec4 c = s * vColor;
    // MC's PARTICLE cutout threshold (alpha test 0.1) — applied to the
    // blended pass so fully transparent texels never write.
    if (c.a < 0.1) discard;
    c.rgb *= uEntityLight;
    vec3 fogDelta = vRenderPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    c.rgb = mix(c.rgb, uFogColor.rgb, fogValue * uFogColor.a);
    FragColor = c;
}
)";

    MobParticleSystem::MobParticleSystem()
        : m_rng(std::chrono::steady_clock::now().time_since_epoch().count())
        , m_limiterRng(std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x5DEECE66DLL) {
        for (auto& t : m_textures) t = INVALID_TEXTURE;
    }

    MobParticleSystem::~MobParticleSystem() {
        Shutdown();
    }

    void MobParticleSystem::Shutdown() {
        if (!g_renderBackend) return;
        DestroySlots();
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        for (int i = 0; i < kTextureCount; ++i) {
            TextureHandle& t = m_textures[i];
            if (t != INVALID_TEXTURE && IsOwnedTexture(i)) g_renderBackend->DestroyTexture(t);
            t = INVALID_TEXTURE;
        }
        m_particles.clear();
        m_incoming.clear();
    }

    namespace {
        // MC particle sprites are 8x8 pixel art sampled nearest off the
        // particle atlas — plain RGBA (not sRGB; the fixed-function-era
        // pipeline these textures were authored for never converted).
        TextureHandle LoadParticleSprite(const char* path) {
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(path, &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[MobParticleSystem] stbi_load failed for %s: %s",
                             path, stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(
                w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex,
                    TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(tex,
                    TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return tex;
        }
    } // namespace

    // Sprite sheets — one PNG per frame (assets/textures/particle/, MC's
    // own particle textures, a resource pack's when one supplies them).
    // Missing files are non-fatal: the frame simply doesn't draw.
    void MobParticleSystem::LoadSprites() {
        auto load = [](const std::string& rel) { return LoadParticleSprite(PlatformMain::GetAssetPath(rel).c_str()); };
        char path[128];
        m_textures[kTexHeart] = load("assets/textures/particle/heart.png");
        m_textures[kTexAngry] = load("assets/textures/particle/angry.png");
        for (int i = 0; i < 8; ++i) {
            std::snprintf(path, sizeof(path), "assets/textures/particle/generic_%d.png", i);
            m_textures[kTexGeneric0 + i] = load(path);
        }
        for (int i = 0; i < 16; ++i) {
            std::snprintf(path, sizeof(path), "assets/textures/particle/explosion_%d.png", i);
            m_textures[kTexExplosion0 + i] = load(path);
        }
        for (int i = 0; i < 8; ++i) {
            std::snprintf(path, sizeof(path), "assets/textures/particle/spell_%d.png", i);
            m_textures[kTexSpell0 + i] = load(path);
        }
        // MC particles/pause_mob_growth.json and reset_mob_growth.json both
        // name the one `glint` sprite.
        m_textures[kTexGlint] = load("assets/textures/particle/glint.png");
        // MC particles/flame.json: the one `flame` sprite.
        m_textures[kTexFlame] = load("assets/textures/particle/flame.png");
        // The potent sulfur geyser (26.3): particles/sulfur_bubbles.json names
        // the one `bubble_white` sprite; noxious_gas, geyser_base, geyser_poof
        // and geyser_plume.json list their eight frames _01.._08 ascending.
        m_textures[kTexBubbleWhite] = load("assets/textures/particle/bubble_white.png");
        const struct { int first; const char* name; } sheets[] = {
            { kTexNoxiousGas0,  "noxious_gas"  },
            { kTexGeyserBase0,  "geyser_base"  },
            { kTexGeyserPoof0,  "geyser_poof"  },
            { kTexGeyserPlume0, "geyser_plume" },
        };
        for (const auto& sheet : sheets) {
            for (int i = 0; i < 8; ++i) {
                std::snprintf(path, sizeof(path), "assets/textures/particle/%s_%02d.png", sheet.name, i + 1);
                m_textures[sheet.first + i] = load(path);
            }
        }
    }

    void MobParticleSystem::ReloadTextures() {
        if (!g_renderBackend || m_shader == INVALID_SHADER) return;
        for (int i = 0; i < kTextureCount; ++i) {
            TextureHandle& t = m_textures[i];
            if (t != INVALID_TEXTURE && IsOwnedTexture(i)) g_renderBackend->DestroyTexture(t);
            t = INVALID_TEXTURE;
        }
        LoadSprites();
    }

    bool MobParticleSystem::Initialize() {
        if (!g_renderBackend) return false;

        if (g_renderBackend->GetType() == BackendType::OpenGL) {
            m_shader = g_renderBackend->CreateShader(s_vertSource, s_fragSource);
        } else {
            // Vulkan: precompiled SPIR-V (shaders/mob_particle_vk.*.spv) on
            // the portal pipeline layout — the fragment shader reads the
            // frame's fog from the Common UBO (EntityEnvironment.hpp); the
            // block vertex layout as the rest of the VK pipeline.
            m_shader = EntityEnvironment::CreateShader(
                "shaders/mob_particle.vert", "shaders/mob_particle.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[MobParticleSystem] Failed to load shader for backend %s",
                         g_renderBackend->GetName());
            return false;
        }

        LoadSprites();

        // The streaming buffers are made on first use (AcquireSlot).
        return true;
    }

    MobParticleSystem::StreamSlot& MobParticleSystem::AcquireSlot(size_t vertsNeeded, size_t minCapacity) {
        StreamSlot& slot = m_slots[m_slotCursor];
        m_slotCursor = (m_slotCursor + 1) % kStreamSlots;
        if (slot.vb == INVALID_BUFFER || slot.capacityVerts < vertsNeeded) {
            size_t newCap = std::max(slot.capacityVerts, minCapacity);
            while (newCap < vertsNeeded) newCap *= 2;
            // Deferred: the previous frame's draw from this slot may still
            // be reading it.
            if (slot.mesh != INVALID_MESH)  g_renderBackend->DeferredDestroyMesh(slot.mesh);
            if (slot.vb   != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slot.vb);
            slot.vb   = g_renderBackend->CreateBuffer(BufferUsage::Vertex, newCap * 24, nullptr,
                                                      BufferAccess::Streaming);
            slot.mesh = g_renderBackend->CreateMesh(slot.vb, INVALID_BUFFER, GetBlockVertexLayout());
            slot.capacityVerts = newCap;
        }
        return slot;
    }

    void MobParticleSystem::DestroySlots() {
        for (StreamSlot& slot : m_slots) {
            if (slot.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(slot.mesh);  slot.mesh = INVALID_MESH; }
            if (slot.vb   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(slot.vb); slot.vb = INVALID_BUFFER; }
            slot.capacityVerts = 0;
        }
        m_slotCursor = 0;
    }

    // ── Spawning — the MC particle constructors, constants verbatim ───────

    // MC ParticleType.getOverrideLimiter(). EXPLOSION_EMITTER (spawned through
    // addAlwaysVisibleParticle, LevelEventHandler:340) — a distant blast
    // keeps its fireball and loses only the debris — and the four geyser
    // types, which ParticleTypes registers with overrideLimiter true, so a
    // plume shows from anywhere its seed was spawned.
    bool MobParticleSystem::OverridesParticleLimiter(Game::ParticleKind kind) {
        switch (kind) {
            case Game::ParticleKind::ExplosionEmitter:
            case Game::ParticleKind::Geyser:
            case Game::ParticleKind::GeyserBase:
            case Game::ParticleKind::GeyserPoof:
            case Game::ParticleKind::GeyserPlume:
                return true;
            default:
                return false;
        }
    }

    // The addAlwaysVisibleParticle callers: the explosion emitter, Potent-
    // SulfurBlock.animateTick's SULFUR_BUBBLES and NoxiousGasCloudParticle's
    // NOXIOUS_GAS. The latter two do NOT override the 32-block limiter
    // (their types register overrideLimiter false); they only get the
    // Minimal setting's reprieve.
    bool MobParticleSystem::AlwaysShown(Game::ParticleKind kind) {
        return kind == Game::ParticleKind::ExplosionEmitter ||
               kind == Game::ParticleKind::SulfurBubbles ||
               kind == Game::ParticleKind::NoxiousGas;
    }

    bool MobParticleSystem::ShouldSpawn(const Client::ClientLevelBridge::QueuedParticle& q,
                                        const glm::vec3& cameraPos) {
        using Game::ParticleStatus;
        const bool overrideLimiter = OverridesParticleLimiter(q.kind);
        const bool alwaysShow = AlwaysShown(q.kind);   // addAlwaysVisibleParticle

        // calculateParticleLevel
        ParticleStatus status = m_particleStatus;
        if (alwaysShow && status == ParticleStatus::Minimal && m_limiterRng.NextInt(10) == 0) {
            status = ParticleStatus::Decreased;
        }
        if (status == ParticleStatus::Decreased && m_limiterRng.NextInt(3) == 0) {
            status = ParticleStatus::Minimal;
        }

        if (overrideLimiter) return true;
        const double dx = q.x - static_cast<double>(cameraPos.x);
        const double dy = q.y - static_cast<double>(cameraPos.y);
        const double dz = q.z - static_cast<double>(cameraPos.z);
        if (dx * dx + dy * dy + dz * dz > kParticleCutoffSq) return false;
        return status != ParticleStatus::Minimal;
    }

    void MobParticleSystem::SpawnFromRequest(
        const Client::ClientLevelBridge::QueuedParticle& q,
        Game::DimensionId dimension) {
        // Reserve headroom for the always-visible kinds.
        //
        // A flat cap refused an ExplosionEmitter exactly as readily as a smoke
        // puff. SpawnExplosionVisualEffects queues one emitter plus up to 512
        // debris per blast, so in a chain the first ~32 blasts consumed the
        // whole pool and every blast after that got NOTHING — not even its
        // fireball. MC's cap is per particle TYPE
        // (ParticleEngine.MAX_PARTICLES_PER_TYPE), so splitting the threshold
        // converges toward it rather than away.
        const size_t limit = OverridesParticleLimiter(q.kind)
                                 ? kMaxParticles
                                 : kMaxParticles - kReservedForOverriding;
        if (m_particles.size() >= limit) return;

        Game::JavaRandom& rng = m_rng;
        Particle p;
        p.kind = q.kind;
        p.x = q.x; p.y = q.y; p.z = q.z;

        // MC Particle(level, x, y, z): default friction 0.98, gravity 0,
        // lifetime = 4 / (rand*0.9 + 0.1) ticks.
        p.lifetime = static_cast<int>(4.0f / (rng.NextFloat() * 0.9f + 0.1f));

        // MC Particle(level, x, y, z, xa, ya, za): jittered, normalised,
        // rescaled velocity. Kept as a lambda so each type applies it (or
        // not) exactly where its constructor chain does.
        auto randomizeVelocity = [&](double xa, double ya, double za) {
            p.xd = xa + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.4f);
            p.yd = ya + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.4f);
            p.zd = za + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.4f);
            const double speed = static_cast<double>(
                (rng.NextFloat() + rng.NextFloat() + 1.0f) * 0.15f);
            const double dd =
                std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd);
            p.xd = p.xd / dd * speed * 0.4;
            p.yd = p.yd / dd * speed * 0.4 + 0.1;
            p.zd = p.zd / dd * speed * 0.4;
        };

        // MC SingleQuadParticle: quadSize = 0.1 * (rand*0.5 + 0.5) * 2.
        auto initQuadSize = [&]() {
            p.quadSize = 0.1f * (rng.NextFloat() * 0.5f + 0.5f) * 2.0f;
        };

        switch (q.kind) {
            case Game::ParticleKind::Heart:
            case Game::ParticleKind::AngryVillager: {
                // MC HeartParticle(level, x, y, z): 7-arg base with (0,0,0)
                // — the addParticle velocity args are DISCARDED (the
                // provider never forwards them), then damped to 1% with a
                // +0.1 rise. AngryVillagerProvider additionally spawns at
                // y + 0.5 and uses the angry sprite.
                if (q.kind == Game::ParticleKind::AngryVillager) p.y += 0.5;
                randomizeVelocity(0.0, 0.0, 0.0);
                initQuadSize();
                p.speedUpWhenYMotionIsBlocked = true;
                p.friction = 0.86f;
                p.xd *= 0.01; p.yd *= 0.01; p.zd *= 0.01;
                p.yd += 0.1;
                p.quadSize *= 1.5f;
                p.lifetime = 16;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::Smoke:
            case Game::ParticleKind::LargeSmoke: {
                // MC SmokeParticle → BaseAshSmokeParticle(level, x, y, z,
                // 0.1, 0.1, 0.1, xa, ya, za, scale, sprites, 0.3, 8, -0.1,
                // true); LargeSmokeParticle is the same at scale 2.5.
                const float scale =
                    q.kind == Game::ParticleKind::LargeSmoke ? 2.5f : 1.0f;
                randomizeVelocity(0.0, 0.0, 0.0);
                initQuadSize();
                p.friction = 0.96f;
                p.gravity = -0.1f;
                p.speedUpWhenYMotionIsBlocked = true;
                p.xd *= 0.1; p.yd *= 0.1; p.zd *= 0.1;   // dirX/dirY/dirZ
                p.xd += q.vx; p.yd += q.vy; p.zd += q.vz;
                const float col = rng.NextFloat() * 0.3f;  // colorRandom
                p.rCol = col; p.gCol = col; p.bCol = col;
                p.quadSize *= 0.75f * scale;
                p.lifetime = static_cast<int>(
                    8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2) *
                    static_cast<double>(scale));
                p.lifetime = std::max(p.lifetime, 1);
                p.hasPhysics = true;
                break;
            }
            case Game::ParticleKind::HushMist: {
                // Aurelith's river mist (engine kind): a slow, faint violet
                // puff hugging the water. One frame of the generic smoke
                // sheet kept for life (a full one, 5..7), a quarter to a
                // half of a block across at first, swelling as it drifts;
                // the exact velocity the water hands it, damped; no
                // collision (it is haze, not a thing).
                p.spriteFrame = static_cast<uint8_t>(5 + rng.NextInt(3));
                p.xd = q.vx; p.yd = q.vy; p.zd = q.vz;
                p.quadSize = 0.55f + rng.NextFloat() * 0.45f;
                const float br = 0.75f + rng.NextFloat() * 0.25f;
                p.rCol = br * 0.62f; p.gCol = br * 0.42f; p.bCol = br * 1.0f;
                p.alpha = 0.0f;
                p.lifetime = 140 + rng.NextInt(80);
                p.gravity = 0.0f;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::VesperGlint: {
                // A glint skimming the river's surface (engine kind): the
                // glint sprite, tiny, pale violet-cyan, a short life that
                // twinkles (the alpha is a fast flicker under a fade).
                initQuadSize();
                p.xd = q.vx; p.yd = q.vy; p.zd = q.vz;
                p.quadSize = 0.035f + rng.NextFloat() * 0.035f;
                const float cyan = rng.NextFloat();
                p.rCol = 0.72f - 0.25f * cyan; p.gCol = 0.70f + 0.28f * cyan; p.bCol = 1.0f;
                p.alpha = 0.0f;
                p.lifetime = 20 + rng.NextInt(24);
                p.gravity = 0.0f;
                p.hasPhysics = false;
                p.spriteFrame = static_cast<uint8_t>(rng.NextInt(256));   // the twinkle's phase
                break;
            }
            case Game::ParticleKind::Flame: {
                // MC FlameParticle → RisingParticle(level, x, y, z, xd, yd,
                // zd, sprite): the 7-arg base (jittered, normalised
                // velocity), damped to 1% plus the passed velocity, a +-0.05
                // position jitter, friction 0.96, lifetime 8 / (r*0.8 + 0.2)
                // + 4. FlameParticle.move skips collision (hasPhysics false).
                randomizeVelocity(q.vx, q.vy, q.vz);
                initQuadSize();
                p.friction = 0.96f;
                p.xd = p.xd * 0.009999999776482582 + q.vx;
                p.yd = p.yd * 0.009999999776482582 + q.vy;
                p.zd = p.zd * 0.009999999776482582 + q.vz;
                p.x += static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.05f);
                p.y += static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.05f);
                p.z += static_cast<double>((rng.NextFloat() - rng.NextFloat()) * 0.05f);
                p.lifetime = static_cast<int>(
                    8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2)) + 4;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::Poof: {
                // MC ExplodeParticle(level, x, y, z, xa, ya, za, sprites):
                // 4-arg base (no velocity jitter), then ±0.05 jitter around
                // the passed velocity.
                initQuadSize();   // overwritten below, but the draw happens in MC too
                p.gravity = -0.1f;
                p.friction = 0.9f;
                p.xd = q.vx + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                p.yd = q.vy + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                p.zd = q.vz + static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.05f);
                const float col = rng.NextFloat() * 0.3f + 0.7f;
                p.rCol = col; p.gCol = col; p.bCol = col;
                p.quadSize = 0.1f * (rng.NextFloat() * rng.NextFloat() * 6.0f + 1.0f);
                p.lifetime = static_cast<int>(
                    16.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2)) + 2;
                break;
            }
            case Game::ParticleKind::Explosion: {
                // MC HugeExplosionParticle(level, x, y, z, size, sprites) —
                // `size` rides the vx slot (exactly how the addParticle xd
                // argument reaches the provider in MC). Never moves.
                initQuadSize();
                p.lifetime = 6 + rng.NextInt(4);
                const float col = rng.NextFloat() * 0.6f + 0.4f;
                p.rCol = col; p.gCol = col; p.bCol = col;
                p.quadSize = 2.0f * (1.0f - static_cast<float>(q.vx) * 0.5f);
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::ExplosionEmitter: {
                // MC HugeExplosionSeedParticle — invisible 8-tick seed that
                // sprays EXPLOSION particles (see TickParticle).
                p.lifetime = 8;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::EntityEffect:
            case Game::ParticleKind::WitchMagic: {
                // MC SpellParticle(level, x, y, z, xa, ya, za, sprites):
                // 7-arg base with (0.5 - rand, ya, 0.5 - rand), then the
                // vertical damp and the standing-still horizontal damp.
                const double ax = 0.5 - rng.NextDouble();
                const double az = 0.5 - rng.NextDouble();
                randomizeVelocity(ax, q.vy, az);
                initQuadSize();
                p.friction = 0.96f;
                p.gravity = -0.1f;
                p.speedUpWhenYMotionIsBlocked = true;
                p.yd *= 0.2;
                if (q.vx == 0.0 && q.vz == 0.0) {
                    p.xd *= 0.1;
                    p.zd *= 0.1;
                }
                p.quadSize *= 0.75f;
                p.lifetime = static_cast<int>(
                    8.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.hasPhysics = false;
                if (q.kind == Game::ParticleKind::WitchMagic) {
                    // MC SpellParticle.WitchProvider: rand*0.5 + 0.35
                    // brightness on the (1, 0, 1) magenta.
                    const float b = rng.NextFloat() * 0.5f + 0.35f;
                    p.rCol = 1.0f * b; p.gCol = 0.0f; p.bCol = 1.0f * b;
                } else {
                    // MC ColorParticleOption tint.
                    p.rCol = q.r; p.gCol = q.g; p.bCol = q.b; p.alpha = q.a;
                }
                break;
            }
            case Game::ParticleKind::FallingDust: {
                // MC FallingDustParticle. Note it does NOT call the velocity
                // randomiser: falling dust starts still and is pulled down by
                // its own hand-rolled gravity below, which is why it trickles
                // straight rather than puffing outward like smoke.
                initQuadSize();
                p.quadSize *= 0.67499995f;
                // lifetime = max(0.9 * (32 / (rand*0.8 + 0.2)), 1).
                const int baseLifetime = static_cast<int>(
                    32.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.lifetime = std::max(static_cast<int>(
                    static_cast<float>(baseLifetime) * 0.9f), 1);
                p.xd = p.yd = p.zd = 0.0;
                // MC clamps yd itself (see TickParticle) rather than using the
                // shared gravity/friction path, so both are neutral here.
                p.gravity  = 0.0f;
                p.friction = 1.0f;
                p.hasPhysics = true;
                p.rollSpeed = (rng.NextFloat() - 0.5f) * 0.1f;
                p.roll = p.oRoll =
                    rng.NextFloat() * 2.0f * 3.14159265358979323846f;
                // The block's dust colour, from AddColorParticle.
                p.rCol = q.r; p.gCol = q.g; p.bCol = q.b; p.alpha = q.a;
                break;
            }
            case Game::ParticleKind::HappyVillager: {
                // MC SuspendedTownParticle.HappyVillagerProvider: the 7-arg
                // base (jittered velocity), a 0.2..0.3 grey the provider
                // overrides to white, quad × (rand*0.6+0.5), velocity damped
                // to 2%, lifetime 20 / (rand*0.8 + 0.2); tick: 0.99 drag, no
                // gravity, no collision (its move is a plain box shift).
                randomizeVelocity(q.vx, q.vy, q.vz);
                initQuadSize();
                (void)rng.NextFloat();   // the base grey, overridden by setColor(1,1,1)
                p.rCol = 1.0f; p.gCol = 1.0f; p.bCol = 1.0f;
                p.quadSize *= rng.NextFloat() * 0.6f + 0.5f;
                p.xd *= 0.02; p.yd *= 0.02; p.zd *= 0.02;
                p.lifetime = static_cast<int>(20.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2));
                p.friction = 0.99f;
                p.gravity = 0.0f;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::PauseMobGrowth:
            case Game::ParticleKind::ResetMobGrowth: {
                // MC SimpleVerticalParticle(level, x, y, z, xa, ya, za,
                // sprite, upwards): the 7-arg base draws and jitters a
                // velocity, then the constructor overwrites it with the
                // exact (xa, ya, za) — zero from AgeableMob — sets gravity
                // 0, nudges yd by ±0.03 (down for PAUSE, up for RESET),
                // scales the quad by rand*0.6+0.5 and lives 8 ticks.
                // OPAQUE layer, physics on (the base default).
                randomizeVelocity(q.vx, q.vy, q.vz);
                initQuadSize();
                p.xd = q.vx; p.yd = q.vy; p.zd = q.vz;
                p.gravity = 0.0f;
                p.yd += (q.kind == Game::ParticleKind::ResetMobGrowth) ? 0.03 : -0.03;
                p.quadSize *= rng.NextFloat() * 0.6f + 0.5f;
                p.lifetime = 8;
                p.hasPhysics = true;
                break;
            }
            case Game::ParticleKind::BlockMarker: {
                // MC BlockMarker(level, x, y, z, state): the 4-arg
                // SingleQuadParticle base (no velocity — the marker never
                // moves; it draws the quad-size random all the same), then
                // gravity 0, lifetime 80, hasPhysics false. getQuadSize is a
                // constant 0.5 regardless of the base's roll.
                initQuadSize();
                p.gravity = 0.0f;
                p.lifetime = 80;
                p.hasPhysics = false;
                p.quadSize = 0.5f;
                // The sprite: getBlockStateModelSet().getParticleMaterial
                // (state).sprite() — the state model's `particle` texture on
                // the blocks atlas (item/barrier for a barrier, item/light_NN
                // for a light at level NN).
                const Game::BlockState state = Game::BlockState::FromRawId(q.blockState);
                const Game::BlockModel& model = Game::BlockRegistry::GetBlockModel(state);
                AtlasUVRect rect;
                if (!g_atlasBuilder ||
                    !g_atlasBuilder->GetUVRect(model.ResolveTexture("particle"), rect)) {
                    return;   // no sprite for this state: nothing to show
                }
                p.u0 = rect.uvMin.x; p.v0 = rect.uvMin.y;
                p.u1 = rect.uvMax.x; p.v1 = rect.uvMax.y;
                break;
            }
            case Game::ParticleKind::HushMote: {
                // The Hush's own: a soft glint (the golden-dandelion sprite,
                // tinted cyan) that drifts on the exact velocity the sweep
                // hands it, fades in and out over a 100..159-tick life, and
                // never touches the collider. Modelled on MC's end-rod
                // particle (SimpleAnimatedParticle: damped drift, colour
                // fade) rather than ported from one — there is no vanilla
                // counterpart for a biome-wide ambient mote at this scale.
                initQuadSize();
                p.xd = q.vx; p.yd = q.vy; p.zd = q.vz;
                p.quadSize = 0.03f + rng.NextFloat() * 0.03f;
                const float br = rng.NextFloat() * 0.4f + 0.6f;
                if (q.r < 1.0f || q.g < 1.0f || q.b < 1.0f) {
                    // Tinted (AddColorParticle): Aurelith's voice-coloured
                    // motes — the Four Voices' colours, the discord's violet.
                    p.rCol = br * q.r; p.gCol = br * q.g; p.bCol = br * q.b;
                } else {
                    p.rCol = br * 0.55f; p.gCol = br * 0.95f; p.bCol = br * 1.0f;
                }
                p.alpha = 0.0f;
                p.lifetime = 100 + rng.NextInt(60);
                p.gravity = 0.0f;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::HushPortal: {
                // MC PortalParticle(level, x, y, z, xd, yd, zd, sprite) — the
                // nether portal's motes, recoloured for the Hush. Its
                // Provider draws the sprite first (`sprite.get(random)`: ONE
                // frame of particles/portal.json's generic_0..7, kept for
                // life — not an age walk), the 4-arg SingleQuadParticle base
                // draws the quad size, and the constructor then overwrites:
                // the exact addParticle velocity (no jitter), quadSize
                // 0.1·(rand·0.2+0.5), a brightness of rand·0.6+0.4 over the
                // tint, and a 40..49-tick life. No physics: tick() is a
                // closed curve back to the start (TickParticle), so the
                // collider never has a say; false keeps it out of the loop.
                //
                // MC's tint is (0.9, 0.3, 1.0) — nether purple. The Hush's
                // portal is sculk teal: #2BD4C0 = (0.17, 0.83, 0.75).
                //
                // MC also brightens the mote with age (getLightCoords adds
                // block emission ramping by (age/lifetime)^4); this renderer
                // has no lightmap and draws every particle fullbright (see
                // the header), so the mote is simply bright from the start.
                p.spriteFrame = static_cast<uint8_t>(rng.NextInt(8));
                initQuadSize();
                p.xd = q.vx; p.yd = q.vy; p.zd = q.vz;
                p.xStart = p.x; p.yStart = p.y; p.zStart = p.z;
                p.quadSize = 0.1f * (rng.NextFloat() * 0.2f + 0.5f);
                const float br = rng.NextFloat() * 0.6f + 0.4f;
                p.rCol = br * 0.17f; p.gCol = br * 0.83f; p.bCol = br * 0.75f;
                p.lifetime = static_cast<int>(rng.NextFloat() * 10.0f) + 40;
                p.gravity = 0.0f;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::AetherPortal:
            case Game::ParticleKind::Portal: {
                // MC PortalParticle again, exactly as HushPortal above — one
                // random frame of the generic sheet, the addParticle velocity
                // verbatim, quadSize 0.1·(rand·0.2+0.5), brightness
                // rand·0.6+0.4 over the tint, a 40..49-tick life, no physics.
                // The tints: vanilla PORTAL's (0.9, 0.3, 1.0) for the
                // twilight pool (TFPortalBlock.animateTick), and the Aether's
                // AETHER_PORTAL pale blue (0.6, 0.8, 1.0).
                p.spriteFrame = static_cast<uint8_t>(rng.NextInt(8));
                initQuadSize();
                p.xd = q.vx; p.yd = q.vy; p.zd = q.vz;
                p.xStart = p.x; p.yStart = p.y; p.zStart = p.z;
                p.quadSize = 0.1f * (rng.NextFloat() * 0.2f + 0.5f);
                const float br = rng.NextFloat() * 0.6f + 0.4f;
                if (q.kind == Game::ParticleKind::AetherPortal) {
                    p.rCol = br * 0.6f; p.gCol = br * 0.8f; p.bCol = br * 1.0f;
                } else {
                    p.rCol = br * 0.9f; p.gCol = br * 0.3f; p.bCol = br * 1.0f;
                }
                p.lifetime = static_cast<int>(rng.NextFloat() * 10.0f) + 40;
                p.gravity = 0.0f;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::SulfurBubbles: {
                // MC SulfurBubbleParticle(level, x, y, z, xa, za, sprite) —
                // its Provider hands (xAux, yAux) in as (xa, za). The 4-arg
                // SingleQuadParticle base: no velocity jitter, the quad size
                // drawn and then replaced. Rises on negative gravity through
                // 0.85 friction for (at most) three blocks; the tick removes
                // it once it leaves the water, tops out or stops rising, so
                // the lifetime is effectively forever.
                initQuadSize();
                p.gravity = -0.04f;
                p.friction = 0.85f;
                p.bbWidth = p.bbHeight = 0.02f;   // setSize(0.02F, 0.02F)
                p.xd = q.vx * 0.20000000298023224 +
                       static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.zd = q.vy * 0.20000000298023224 +
                       static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.02f);
                p.sizeMin = 0.02f + 0.02f * rng.NextFloat();   // sizeStart
                p.quadSize = p.sizeMin;
                p.lifetime = INT_MAX;
                p.yStart = p.y;              // yStart = yo
                p.yEnd = p.y + 4.0 - 1.0;
                p.yPrev = p.y;
                break;
            }
            case Game::ParticleKind::NoxiousGas: {
                // MC NoxiousGasParticle → BaseAshSmokeParticle(level, x, y, z,
                // 0.1, 0.1, 0.1, xa, ya, za, 3.0, sprites, 0.3, 5, -0.02,
                // true); then white, a lifetime of 6 / (r·0.5 + 0.5) · scale
                // and a fade over its second half (TickParticle).
                constexpr float scale = 3.0f;
                randomizeVelocity(0.0, 0.0, 0.0);
                initQuadSize();
                p.friction = 0.96f;
                p.gravity = -0.02f;
                p.speedUpWhenYMotionIsBlocked = true;
                p.xd *= 0.1; p.yd *= 0.1; p.zd *= 0.1;
                p.xd += q.vx; p.yd += q.vy; p.zd += q.vz;
                (void)rng.NextFloat();   // the base's colorRandom grey, overridden below
                p.quadSize *= 0.75f * scale;
                p.lifetime = std::max(static_cast<int>(
                    5.0 / (static_cast<double>(rng.NextFloat()) * 0.8 + 0.2) *
                    static_cast<double>(scale)), 1);
                p.hasPhysics = true;
                p.rCol = 1.0f; p.gCol = 1.0f; p.bCol = 1.0f;
                p.lifetime = static_cast<int>(
                    6.0 / (static_cast<double>(rng.NextFloat()) * 0.5 + 0.5) *
                    static_cast<double>(scale));
                p.fadeStart = static_cast<float>(p.lifetime) / 2.0f;
                break;
            }
            case Game::ParticleKind::NoxiousGasCloud: {
                // MC NoxiousGasCloudParticle: an unrendered 20-tick seed at
                // rest (the 4-arg base), puffing NOXIOUS_GAS (TickParticle).
                p.lifetime = 20;
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::Geyser: {
                // MC GeyserEruptionParticle: an unrendered 20-tick seed at
                // rest that throws the base, the plume and the poof
                // (TickParticle). GeyserParticleOptions.waterBlocks rides vx.
                p.lifetime = 20;
                p.waterBlocks = static_cast<int>(q.vx);
                p.hasPhysics = false;
                break;
            }
            case Game::ParticleKind::GeyserBase:
            case Game::ParticleKind::GeyserPoof: {
                // MC GeyserBaseParticle's Provider: ±0.25 around the seed,
                // 0.2 up. Then BaseAshSmokeParticle(level, x, y, z, burst,
                // burst, burst, xa, ya, za, size, sprites, 0.0, 0, 0.0, true)
                // with burst = base + 0.25·water (base 1.5 for GEYSER_BASE,
                // 2.0 for GEYSER_POOF) and size = 3 + 0.125·water; then
                // white, friction 0.725, only ever rising (|yd|), 20..25
                // ticks. The seed forwards its own velocity, which is zero.
                p.x += static_cast<double>((rng.NextFloat() - 0.5f) * 0.5f);
                p.y += static_cast<double>((rng.NextFloat() - 0.5f) * 0.5f) + 0.20000000298023224;
                p.z += static_cast<double>((rng.NextFloat() - 0.5f) * 0.5f);
                const int water = static_cast<int>(q.vx);
                const float burstBase = q.kind == Game::ParticleKind::GeyserPoof ? 2.0f : 1.5f;
                const float burst = burstBase + 0.25f * static_cast<float>(water);
                const float size = 3.0f + 0.125f * static_cast<float>(water);
                randomizeVelocity(0.0, 0.0, 0.0);
                initQuadSize();
                p.friction = 0.96f;
                p.gravity = 0.0f;
                p.speedUpWhenYMotionIsBlocked = true;
                p.xd *= static_cast<double>(burst);
                p.yd *= static_cast<double>(burst);
                p.zd *= static_cast<double>(burst);
                (void)rng.NextFloat();   // colorRandom 0: a black base, overridden below
                p.quadSize *= 0.75f * size;
                (void)rng.NextFloat();   // maxLifetime 0: lifetime max(0, 1), replaced below
                p.hasPhysics = true;
                p.friction = 0.725f;
                p.rCol = 1.0f; p.gCol = 1.0f; p.bCol = 1.0f;
                p.yd = std::abs(p.yd);
                const float lifetimeFactor = 0.8f + 0.2f * rng.NextFloat();
                p.lifetime = static_cast<int>(25.0f * lifetimeFactor);
                break;
            }
            case Game::ParticleKind::GeyserPlume: {
                // MC GeyserPlumeParticle's Provider: ±0.1 across, 0..1 up.
                // Then the 8-arg SingleQuadParticle base (a jittered
                // velocity, the quad size drawn), and: a plume 5 blocks per
                // water block tall, driven up by an initial propulsion
                // (negative gravity) that TickParticle turns into a
                // growing pull as it climbs, a slight horizontal spray,
                // friction 1, and a quad growing from minSize to maxSize.
                p.x += static_cast<double>((rng.NextFloat() - 0.5f) * 0.2f);
                p.y += static_cast<double>(rng.NextFloat());
                p.z += static_cast<double>((rng.NextFloat() - 0.5f) * 0.2f);
                const int water = static_cast<int>(q.vx);
                randomizeVelocity(0.0, 0.0, 0.0);
                initQuadSize();
                const int plumeHeight = 5 * std::max(1, water);
                p.hasPhysics = true;
                p.speedUpWhenYMotionIsBlocked = true;
                p.lifetime = plumeHeight * 5;
                p.yd = 0.0;
                p.yStart = p.y;
                p.yEnd = p.yStart + static_cast<double>(plumeHeight) - 1.0;
                p.sprayX = (rng.NextFloat() - 0.5f) * 0.2f;
                p.sprayZ = (rng.NextFloat() - 0.5f) * 0.2f;
                p.friction = 1.0f;
                p.propulsion = (water == 1 ? 1.5f : 1.0f) * static_cast<float>(plumeHeight) * 1.45f;
                p.gravity = -p.propulsion;
                const float initiallyRandomizedSize = p.quadSize * 0.75f;
                p.sizeMin = initiallyRandomizedSize * (2.0f + static_cast<float>(plumeHeight) / 8.0f);
                p.sizeMax = initiallyRandomizedSize * (3.0f + static_cast<float>(plumeHeight) / 8.0f);
                p.quadSize = p.sizeMin;
                break;
            }
        }

        p.xo = p.x; p.yo = p.y; p.zo = p.z;
        p.dimension = dimension;
        m_particles.push_back(p);
    }

    // ── Simulation — MC Particle.tick / move at fixed 20 Hz ────────────────

    void MobParticleSystem::MoveParticle(Particle& p, double xa, double ya,
                                         double za,
                                         const Game::IBlockAccess* blocks) {
        // MC Particle.move, verbatim structure.
        if (p.stoppedByCollision) return;

        const double origXa = xa, origYa = ya, origZa = za;
        // MC MAXIMUM_COLLISION_VELOCITY_SQUARED = 100².
        if (p.hasPhysics && blocks != nullptr &&
            (xa != 0.0 || ya != 0.0 || za != 0.0) &&
            xa * xa + ya * ya + za * za < 10000.0) {
            // Particle AABB: setSize(bbWidth, bbHeight) around (x, y feet,
            // z) — 0.2 × 0.2 for all but the sulfur bubble.
            const double hw = static_cast<double>(p.bbWidth) * 0.5;
            Game::AABBd box;
            box.min = glm::dvec3(p.x - hw, p.y, p.z - hw);
            box.max = glm::dvec3(p.x + hw, p.y + static_cast<double>(p.bbHeight), p.z + hw);

            Game::AABBd region = box;
            region.min += glm::dvec3(std::min(xa, 0.0), std::min(ya, 0.0),
                                     std::min(za, 0.0));
            region.max += glm::dvec3(std::max(xa, 0.0), std::max(ya, 0.0),
                                     std::max(za, 0.0));

            Game::PhysicsContext ctx;
            ctx.blockAccess = blocks;
            // Its OWN buffer, not the thread_local one MoveEntity uses — both
            // run on the client main thread, so sharing would alias.
            //
            // Declaring this inside the per-particle collision branch was the
            // measured 17x swing in this system: 0.016 ms/tick with nothing
            // solid in range, 0.269 ms/tick with everything in range, driven
            // purely by whether push_back ever fired.
            static thread_local std::vector<Game::AABBd> colliders;
            Game::CollectBlockColliders(region, ctx, colliders);

            if (!colliders.empty()) {
                // MC Entity.collideWithShapes: Y first, then the larger
                // horizontal component.
                ya = Game::CollideAxis(1, box, ya, colliders);
                box.min.y += ya; box.max.y += ya;
                const bool zBigger = std::abs(xa) < std::abs(za);
                if (zBigger) {
                    za = Game::CollideAxis(2, box, za, colliders);
                    box.min.z += za; box.max.z += za;
                    xa = Game::CollideAxis(0, box, xa, colliders);
                } else {
                    xa = Game::CollideAxis(0, box, xa, colliders);
                    box.min.x += xa; box.max.x += xa;
                    za = Game::CollideAxis(2, box, za, colliders);
                }
            }
        }

        p.x += xa; p.y += ya; p.z += za;

        if (std::abs(origYa) >= 1.0e-5 && std::abs(ya) < 1.0e-5) {
            p.stoppedByCollision = true;
        }
        p.onGround = origYa != ya && origYa < 0.0;
        if (origXa != xa) p.xd = 0.0;
        if (origZa != za) p.zd = 0.0;
    }

    void MobParticleSystem::TickParticle(
        Particle& p, const Game::IBlockAccess* blocks,
        std::vector<Client::ClientLevelBridge::QueuedParticle>& emitterSpawns) {
        p.xo = p.x; p.yo = p.y; p.zo = p.z;

        if (p.kind == Game::ParticleKind::ExplosionEmitter) {
            // MC HugeExplosionSeedParticle.tick: 6 EXPLOSION particles at
            // ±4-block offsets, `size` = age/lifetime, then age — removal
            // exactly at age == lifetime.
            for (int i = 0; i < 6; ++i) {
                const double xx =
                    p.x + (m_rng.NextDouble() - m_rng.NextDouble()) * 4.0;
                const double yy =
                    p.y + (m_rng.NextDouble() - m_rng.NextDouble()) * 4.0;
                const double zz =
                    p.z + (m_rng.NextDouble() - m_rng.NextDouble()) * 4.0;
                emitterSpawns.push_back(
                    {Game::ParticleKind::Explosion, xx, yy, zz,
                     static_cast<double>(p.age) / static_cast<double>(p.lifetime),
                     0.0, 0.0, 1.0f, 1.0f, 1.0f, 1.0f});
            }
            ++p.age;
            if (p.age == p.lifetime) p.removed = true;
            return;
        }

        if (p.kind == Game::ParticleKind::NoxiousGasCloud ||
            p.kind == Game::ParticleKind::Geyser) {
            // The 26.3 seeds (NoRenderParticle at rest): Particle.tick ages
            // them out at the lifetime, and their own tick runs after it
            // regardless — the removing tick emits too, as MC's does.
            if (p.age++ >= p.lifetime) p.removed = true;
            if (p.kind == Game::ParticleKind::NoxiousGasCloud) {
                // MC NoxiousGasCloudParticle.tick: every other tick, a random
                // point up to 3 blocks out from the source cell's centre and
                // a quarter block down; a NOXIOUS_GAS puff there when the gas
                // can reach it (addAlwaysVisibleParticle).
                if (p.age % 2 == 0 && blocks) {
                    const glm::ivec3 source(static_cast<int>(std::floor(p.x)),
                                            static_cast<int>(std::floor(p.y)),
                                            static_cast<int>(std::floor(p.z)));
                    glm::dvec3 dir(static_cast<double>(m_rng.NextFloat() - 0.5f), 0.0,
                                   static_cast<double>(m_rng.NextFloat() - 0.5f));
                    const double length = std::sqrt(dir.x * dir.x + dir.z * dir.z);
                    dir = length < static_cast<double>(1.0e-5f) ? glm::dvec3(0.0) : dir / length;
                    const float distance = m_rng.NextFloat() * 3.0f;
                    const glm::dvec3 spawn =
                        glm::dvec3(source.x + 0.5, source.y + 0.5, source.z + 0.5) +
                        dir * static_cast<double>(distance) - glm::dvec3(0.0, 0.25, 0.0);
                    if (Game::PotentSulfur::CanBeReachedByNoxiousGas(*blocks, source, spawn)) {
                        emitterSpawns.push_back({Game::ParticleKind::NoxiousGas,
                                                 spawn.x, spawn.y, spawn.z, 0.0, 0.0, 0.0,
                                                 1.0f, 1.0f, 1.0f, 1.0f});
                    }
                }
            } else {
                // MC GeyserEruptionParticle.tick: two GEYSER_BASE every other
                // tick, waterBlocks + 2 GEYSER_PLUME every tick, twenty
                // GEYSER_POOF every tenth — all from the seed's position,
                // carrying its water column (vx) and its zero velocity.
                const double water = static_cast<double>(p.waterBlocks);
                auto emit = [&](Game::ParticleKind kind) {
                    emitterSpawns.push_back({kind, p.x, p.y, p.z, water, 0.0, 0.0,
                                             1.0f, 1.0f, 1.0f, 1.0f});
                };
                if (p.age % 2 == 0) {
                    for (int i = 0; i < 2; ++i) emit(Game::ParticleKind::GeyserBase);
                }
                for (int i = 0; i < p.waterBlocks + 2; ++i) emit(Game::ParticleKind::GeyserPlume);
                if (p.age % 10 == 0) {
                    for (int i = 0; i < 20; ++i) emit(Game::ParticleKind::GeyserPoof);
                }
            }
            return;
        }

        if (p.age++ >= p.lifetime) {
            p.removed = true;
            return;
        }

        if (p.kind == Game::ParticleKind::Explosion) {
            // MC HugeExplosionParticle.tick: ages, never moves.
            return;
        }

        if (p.kind == Game::ParticleKind::HushMote) {
            // Damped drift with a slow bob, no collision; alpha rises over
            // the first eighth of the life and falls over the last quarter.
            p.x += p.xd; p.y += p.yd; p.z += p.zd;
            p.xd *= 0.985; p.yd *= 0.985; p.zd *= 0.985;
            p.yd += 0.00015 * std::sin(static_cast<double>(p.age) * 0.12);
            const float t = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
            p.alpha = std::clamp(std::min(t * 8.0f, (1.0f - t) * 4.0f), 0.0f, 0.9f);
            return;
        }

        if (p.kind == Game::ParticleKind::HushMist) {
            // Drifts with the current, a very slow lift, a lazy sideways
            // wander; faint throughout (at most ~0.2), in over the first
            // fifth, out over the last half.
            p.x += p.xd; p.y += p.yd; p.z += p.zd;
            p.xd *= 0.992; p.zd *= 0.992; p.yd *= 0.98;
            const double wander = static_cast<double>(p.age) * 0.035 + p.spriteFrame;
            p.xd += 0.0004 * std::sin(wander);
            p.zd += 0.0004 * std::cos(wander * 0.8);
            const float t = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
            p.alpha = 0.2f * std::clamp(std::min(t * 5.0f, (1.0f - t) * 2.0f), 0.0f, 1.0f);
            return;
        }

        if (p.kind == Game::ParticleKind::VesperGlint) {
            // Skims, slowing; twinkles — a quick flicker (seeded phase)
            // under a fade in and out.
            p.x += p.xd; p.y += p.yd; p.z += p.zd;
            p.xd *= 0.96; p.yd *= 0.9; p.zd *= 0.96;
            const float t = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
            const float twinkle = 0.55f + 0.45f * std::sin(static_cast<float>(p.age) * 1.7f
                                                           + static_cast<float>(p.spriteFrame) * 0.1f);
            p.alpha = std::clamp(std::min(t * 6.0f, (1.0f - t) * 3.0f), 0.0f, 1.0f) * twinkle;
            return;
        }

        if (p.kind == Game::ParticleKind::HushPortal ||
            p.kind == Game::ParticleKind::AetherPortal ||
            p.kind == Game::ParticleKind::Portal) {
            // MC PortalParticle.tick: no motion integration at all — no
            // gravity, no friction, no move(). The position is a closed
            // curve from the spawn point: with a = age/lifetime,
            //     pos = 1 - (-a + 2a²)
            //     x = xStart + xd·pos,  y = yStart + yd·pos + (1 - a),  z = …
            // so the mote shoots out along its velocity (pos peaks at 1.125
            // a quarter of the way in), is drawn back to where it started
            // by the end, and rises one block over its life on the way.
            const float a = static_cast<float>(p.age) / static_cast<float>(p.lifetime);
            float pos = -a + a * a * 2.0f;
            pos = 1.0f - pos;
            p.x = p.xStart + p.xd * static_cast<double>(pos);
            p.y = p.yStart + p.yd * static_cast<double>(pos) + static_cast<double>(1.0f - a);
            p.z = p.zStart + p.zd * static_cast<double>(pos);
            return;
        }

        if (p.kind == Game::ParticleKind::FallingDust) {
            // MC FallingDustParticle.tick. It does NOT use the shared
            // gravity/friction path below: it moves first, then applies its own
            // acceleration with a hard terminal velocity, which is what makes
            // dust drift down at a constant slow rate rather than accelerating
            // like a thrown particle.
            p.oRoll = p.roll;
            p.roll += 3.14159265358979323846f * p.rollSpeed * 2.0f;
            if (p.onGround) p.oRoll = p.roll = 0.0f;

            MoveParticle(p, p.xd, p.yd, p.zd, blocks);
            p.yd -= 0.003;
            p.yd = std::max(p.yd, -0.14);
            return;
        }

        // MC Particle.tick.
        p.yd -= 0.04 * static_cast<double>(p.gravity);
        MoveParticle(p, p.xd, p.yd, p.zd, blocks);
        if (p.speedUpWhenYMotionIsBlocked && p.y == p.yo) {
            p.xd *= 1.1;
            p.zd *= 1.1;
        }
        p.xd *= static_cast<double>(p.friction);
        p.yd *= static_cast<double>(p.friction);
        p.zd *= static_cast<double>(p.friction);
        if (p.onGround) {
            p.xd *= 0.7;
            p.zd *= 0.7;
        }

        // The subclass halves that run after super.tick().
        if (p.kind == Game::ParticleKind::SulfurBubbles) {
            // MC SulfurBubbleParticle.tick: gone once out of the water
            // (the cell it is in no longer a water source), at the top of
            // its three blocks, or when it stopped rising; else a
            // horizontal wiggle, a second, sideways-only move, and a quad
            // growing towards 0.15 with the climb.
            const bool inWater = blocks &&
                Game::GetFluidState(*blocks, static_cast<int>(std::floor(p.x)),
                                    static_cast<int>(std::floor(p.y)),
                                    static_cast<int>(std::floor(p.z)))
                    .IsSourceOf(Game::FluidType::Water);
            if (!p.removed && !inWater) p.removed = true;
            if (!p.removed && p.y >= p.yEnd) p.removed = true;
            if (!p.removed && p.y <= p.yPrev) p.removed = true;
            const auto wiggle = [&]() {
                return static_cast<double>(m_rng.NextFloat() * 0.003f *
                                           static_cast<float>(m_rng.NextBool() ? 1 : -1)) * 0.5;
            };
            p.xd += wiggle();
            p.zd += wiggle();
            MoveParticle(p, p.xd, 0.0, p.zd, blocks);
            const float travelProgress = static_cast<float>((p.y - p.yStart) / (p.yEnd - p.yStart));
            p.quadSize = p.sizeMin + travelProgress * (0.15f - p.sizeMin);
            p.yPrev = p.y;
        } else if (p.kind == Game::ParticleKind::GeyserPlume) {
            // MC GeyserPlumeParticle.tick: once it falls, passes the top or
            // stalls, five more ticks with no friction left; the pull grows
            // with the cube of the climb, the spray with the climb, the quad
            // from minSize to maxSize.
            if (!p.done && (p.yd < 0.0 || p.y > p.yEnd || p.y == p.yo)) {
                p.lifetime = std::min(p.lifetime, p.age + 5);
                p.friction = 0.0f;
                p.done = true;
            }
            const double yProgressLinear = std::clamp((p.y - p.yStart) / (p.yEnd - p.yStart), 0.0, 1.0);
            const double yProgressExponential = std::pow(yProgressLinear, 3.0);
            p.gravity = p.propulsion * static_cast<float>(yProgressExponential) * 0.12f;
            p.xd = yProgressLinear * static_cast<double>(p.sprayX);
            p.zd = yProgressLinear * static_cast<double>(p.sprayZ);
            p.quadSize = p.sizeMin + static_cast<float>(yProgressLinear *
                                                        static_cast<double>(p.sizeMax - p.sizeMin));
        } else if (p.kind == Game::ParticleKind::NoxiousGas) {
            // MC NoxiousGasParticle.tick: fades out over the second half.
            if (static_cast<float>(p.age) > p.fadeStart) {
                const float framesSinceFadeOutStart = static_cast<float>(p.age) - p.fadeStart;
                p.alpha = (static_cast<float>(p.lifetime) - framesSinceFadeOutStart) /
                          static_cast<float>(p.lifetime);
            }
        }
    }

    glm::vec3 MobParticleSystem::AnchorFor(Game::DimensionId dimension,
                                           const glm::vec3& cameraPos) {
        if (!Client::ClientLevels::HasSession()) return cameraPos;
        if (dimension == Client::ClientLevels::ActiveDimension()) return cameraPos;
#if ENABLE_IMMERSIVE_PORTALS
        // The bound level is the active one here (main thread, between
        // packet drains): its portals are the ones the player looks through.
        const Game::Immersive::Portal* nearest = nullptr;
        double nearestDist = 1e30;
        const glm::dvec3 cam(cameraPos);
        Client::GetClientImmersivePortals().ForEach([&](const Game::Immersive::Portal& p) {
            const Game::DimensionId dest = p.IsMirror() ? p.dimension : p.destDimension;
            if (dest != dimension) return;
            glm::dvec3 mn, mx;
            p.BoundingBox(mn, mx, 0.0);
            const double d = glm::length(glm::clamp(cam, mn, mx) - cam);
            if (d < nearestDist) { nearestDist = d; nearest = &p; }
        });
        if (nearest) return glm::vec3(nearest->TransformPoint(cam));
#endif
        return cameraPos;
    }

    void MobParticleSystem::Update(float dt, const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("MobParticles.Update");
        if (m_shader == INVALID_SHADER) return;

        // 1. Drain queued spawn requests (main thread — see the queue note
        //    in ClientLevelBridge), from EVERY level the client holds: the
        //    one the player stands in and the far sides it sees through
        //    immersive portals. Each particle is stamped with its level.
        //
        //    MC ClientLevel.doAddParticle drops any non-overriding particle
        //    further than 32 blocks from the camera. Without it a big TNT
        //    chain across the map spawned its full 512-particle debris cloud
        //    at every blast, all of it invisible detail the simulation still
        //    paid for every tick. For a far level the distance is measured
        //    from the camera's image through the portal (AnchorFor).
        //
        //    The Particles option (All / Decreased / Minimal) gates the same
        //    call — see ShouldSpawn. Read once here, on the main thread, and
        //    published to each level bridge for the explosion debris spawner,
        //    which consults it from wherever the explosion is processed.
        m_particleStatus = static_cast<Game::ParticleStatus>(
            Platform::g_gameSettings.GetParticles());
        Client::ClientLevels::ForEach([&](Client::ClientLevel& level) {
            Client::ClientMobManager* mobs = level.Mobs();
            if (!mobs) return;
            mobs->SetParticleStatus(m_particleStatus);
            m_incoming.clear();
            mobs->DrainParticles(m_incoming);
            if (m_incoming.empty()) return;
            const glm::vec3 anchor = AnchorFor(level.Dimension(), cameraPos);
            for (const auto& q : m_incoming) {
                if (!ShouldSpawn(q, anchor)) continue;
                SpawnFromRequest(q, level.Dimension());
            }
        });
        m_incoming.clear();

        // The block view each particle collides against — its own level's.
        // Resolved once per Update; a level that vanished mid-frame reads as
        // no collision, which is the same "fly through" the null case is.
        const Game::IBlockAccess* blocksFor[Game::kDimensionCount] = {};
        bool blocksKnown[Game::kDimensionCount] = {};
        const auto blocksOf = [&](Game::DimensionId dim) -> const Game::IBlockAccess* {
            const int slot = Game::DimensionSlot(dim);
            if (!blocksKnown[slot]) {
                blocksKnown[slot] = true;
                Client::ClientLevel* level = Client::ClientLevels::HasSession()
                    ? Client::ClientLevels::Get(dim) : nullptr;
                blocksFor[slot] = level ? level->Blocks() : nullptr;
            }
            return blocksFor[slot];
        };

        // 2. Fixed 20 Hz simulation, MC's tick rate — the ported constants
        //    (friction per tick, 0.04·gravity per tick) only mean anything
        //    at this cadence. Cap the catch-up so a long hitch doesn't
        //    spiral.
        m_tickAccum += dt;
        constexpr float kTick = 0.05f;
        int steps = 0;
        std::vector<Client::ClientLevelBridge::QueuedParticle> emitterSpawns;
        std::vector<Game::DimensionId> emitterDims;
        while (m_tickAccum >= kTick && steps < 5) {
            m_tickAccum -= kTick;
            ++steps;
            emitterSpawns.clear();
            emitterDims.clear();
            for (auto& p : m_particles) {
                if (p.removed) continue;
                const size_t before = emitterSpawns.size();
                TickParticle(p, blocksOf(p.dimension), emitterSpawns);
                // The emitter's children live where the emitter does.
                for (size_t i = before; i < emitterSpawns.size(); ++i) {
                    emitterDims.push_back(p.dimension);
                }
            }
            m_particles.erase(
                std::remove_if(m_particles.begin(), m_particles.end(),
                               [](const Particle& p) { return p.removed; }),
                m_particles.end());
            // The emitter's children join AFTER the sweep — they must not
            // be ticked (or erased) in the tick that spawned them.
            //
            // And they face the SAME distance cull as every other spawn. They
            // used to bypass it by calling SpawnFromRequest directly, which is
            // a divergence FROM MC, not a shortcut: HugeExplosionSeedParticle
            // .tick calls level.addParticle, which routes through
            // ClientLevel.doAddParticle and is subject to the 1024.0 test.
            // Only the seed itself is force-added. A blast 500 blocks away was
            // spawning 48 full-cost particles vanilla drops — 24,576 across a
            // 512-blast chain, enough on its own to exhaust the pool.
            for (size_t i = 0; i < emitterSpawns.size(); ++i) {
                const Game::DimensionId dim = emitterDims[i];
                if (!ShouldSpawn(emitterSpawns[i], AnchorFor(dim, cameraPos))) continue;
                SpawnFromRequest(emitterSpawns[i], dim);
            }
        }
        if (steps == 5) m_tickAccum = 0.0f;
        m_partialTick = std::clamp(m_tickAccum / kTick, 0.0f, 1.0f);
    }

    // ── Rendering ──────────────────────────────────────────────────────────

    int MobParticleSystem::TextureIndexFor(const Particle& p) const {
        // MC SpriteSet.get(age, lifetime) = list[age * (n-1) / lifetime],
        // with each sheet's JSON frame order (generic and spell sheets are
        // listed DESCENDING — smoke plays generic_7 down to generic_0;
        // explosion is listed ascending).
        const int life = std::max(p.lifetime, 1);
        const int a = std::min(p.age, life);
        switch (p.kind) {
            case Game::ParticleKind::Heart:         return kTexHeart;
            case Game::ParticleKind::AngryVillager: return kTexAngry;
            case Game::ParticleKind::Smoke:
            case Game::ParticleKind::LargeSmoke:
            case Game::ParticleKind::Poof:
                return kTexGeneric0 + (7 - a * 7 / life);
            case Game::ParticleKind::Explosion:
                return kTexExplosion0 + a * 15 / life;
            case Game::ParticleKind::EntityEffect:
            case Game::ParticleKind::WitchMagic:
                // MC's entity_effect sheet is effect_0..7; this repo carries
                // the visually-equivalent spell_0..7 (the witch sheet), so
                // both kinds share it — sprite stand-in, physics exact.
                return kTexSpell0 + (7 - a * 7 / life);
            case Game::ParticleKind::FallingDust:
                // MC FallingDustParticle uses the generic_* sheet too, and its
                // Provider passes `sprites.first()` then setSpriteFromAge — the
                // same ascending walk the smoke family does in reverse. Listed
                // descending here for the same reason: the sheet's JSON order.
                return kTexGeneric0 + (7 - a * 7 / life);
            case Game::ParticleKind::BlockMarker:
                return kTexAtlas;
            case Game::ParticleKind::PauseMobGrowth:
            case Game::ParticleKind::ResetMobGrowth:
            case Game::ParticleKind::HushMote:
            case Game::ParticleKind::VesperGlint:
            case Game::ParticleKind::HappyVillager:   // particles/happy_villager.json → glint
                return kTexGlint;
            case Game::ParticleKind::HushMist:
                // One full puff of the generic sheet, kept for life.
                return kTexGeneric0 + std::min<int>(p.spriteFrame, 7);
            case Game::ParticleKind::HushPortal:
            case Game::ParticleKind::AetherPortal:
            case Game::ParticleKind::Portal:
                // MC particles/portal.json is the generic_0..7 sheet too, and
                // PortalParticle's Provider picks one frame at random for
                // the mote's whole life (SpriteSet.get(random)) — no walk.
                return kTexGeneric0 + std::min<int>(p.spriteFrame, 7);
            case Game::ParticleKind::Flame:
                return kTexFlame;
            case Game::ParticleKind::SulfurBubbles:
                return kTexBubbleWhite;
            // The geyser sheets (setSpriteFromAge; their JSONs list _01.._08
            // ascending).
            case Game::ParticleKind::NoxiousGas:
                return kTexNoxiousGas0 + a * 7 / life;
            case Game::ParticleKind::GeyserBase:
                return kTexGeyserBase0 + a * 7 / life;
            case Game::ParticleKind::GeyserPoof:
                return kTexGeyserPoof0 + a * 7 / life;
            case Game::ParticleKind::GeyserPlume:
                return kTexGeyserPlume0 + a * 7 / life;
            case Game::ParticleKind::ExplosionEmitter:
            case Game::ParticleKind::NoxiousGasCloud:
            case Game::ParticleKind::Geyser:
                return -1;   // NoRenderParticle
        }
        return -1;
    }

    float MobParticleSystem::QuadSizeFor(const Particle& p,
                                         float partialTick) const {
        switch (p.kind) {
            case Game::ParticleKind::Heart:
            case Game::ParticleKind::AngryVillager:
            case Game::ParticleKind::Smoke:
            case Game::ParticleKind::LargeSmoke:
            case Game::ParticleKind::FallingDust:
            case Game::ParticleKind::NoxiousGas:     // BaseAshSmokeParticle
            case Game::ParticleKind::GeyserBase:     // BaseAshSmokeParticle
            case Game::ParticleKind::GeyserPoof:     // BaseAshSmokeParticle
                // MC HeartParticle / BaseAshSmokeParticle / FallingDustParticle
                // all share getQuadSize: ramp in over the first 1/32 of the
                // lifetime, so a particle fades IN rather than popping.
                return p.quadSize *
                       std::clamp((static_cast<float>(p.age) + partialTick) /
                                      static_cast<float>(p.lifetime) * 32.0f,
                                  0.0f, 1.0f);
            case Game::ParticleKind::HushPortal:
            case Game::ParticleKind::AetherPortal:
            case Game::ParticleKind::Portal: {
                // MC PortalParticle.getQuadSize: s = 1 - (1 - t)² — the mote
                // starts at nothing and swells to its full size as it is
                // drawn back home, fastest at the start.
                float s = (static_cast<float>(p.age) + partialTick) / static_cast<float>(p.lifetime);
                s = 1.0f - s;
                s *= s;
                s = 1.0f - s;
                return p.quadSize * s;
            }
            case Game::ParticleKind::HushMist: {
                // Swells by half again over its life as it thins.
                const float t = (static_cast<float>(p.age) + partialTick) / static_cast<float>(p.lifetime);
                return p.quadSize * (1.0f + 0.5f * t);
            }
            case Game::ParticleKind::Flame: {
                // MC FlameParticle.getQuadSize: shrinks to half by the end,
                // quadratically.
                const float t = (static_cast<float>(p.age) + partialTick) / static_cast<float>(p.lifetime);
                return p.quadSize * (1.0f - t * t * 0.5f);
            }
            default:
                return p.quadSize;
        }
    }

    void MobParticleSystem::Render(const glm::mat4& projection,
                                   const glm::mat4& view,
                                   const glm::vec3& cameraPos,
                                   Game::DimensionId dimension) {
        PROFILE_ZONE_N("MobParticles.Render");
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        if (m_particles.empty()) return;

        // The blocks atlas, for the block marker. Fetched per draw because a
        // resource-pack reload rebuilds it under a new handle.
        m_textures[kTexAtlas] = g_atlasBuilder ? g_atlasBuilder->GetBackendTextureHandle()
                                               : INVALID_TEXTURE;

        struct Vert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };

        // Camera basis for billboarding (LOOKAT_XYZ — full camera facing).
        const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);

        // Bucket vertices per sprite texture so each texture is one draw —
        // twice over: [tex * 2] the particles lit by the sky, [tex * 2 + 1]
        // the ones MC lights fully (Particle.getLightCoords overrides —
        // HugeExplosionParticle's FULL_BRIGHT, PortalParticle's block light
        // ramping to 15; the portal motes share generic_* with the smoke, so
        // the split cannot follow the texture).
        static thread_local std::vector<Vert> buckets[kTextureCount * 2];
        for (auto& b : buckets) b.clear();
        const auto fullBright = [](const auto& particle, int texIdx) {
            // FlameParticle.getLightCoords: addSmoothBlockEmission ramps the
            // block light to 15 over the life — lit from the first frame.
            return (texIdx >= kTexExplosion0 && texIdx < kTexExplosion0 + 16) ||
                   particle.kind == Game::ParticleKind::Flame ||
                   particle.kind == Game::ParticleKind::Portal ||
                   particle.kind == Game::ParticleKind::HushPortal ||
                   particle.kind == Game::ParticleKind::AetherPortal ||
                   // The river's own light: its mist and glints glow.
                   particle.kind == Game::ParticleKind::HushMist ||
                   particle.kind == Game::ParticleKind::VesperGlint;
        };

        const float pt = m_partialTick;
        for (const auto& p : m_particles) {
            if (p.dimension != dimension) continue;
            const int texIdx = TextureIndexFor(p);
            if (texIdx < 0 || texIdx >= kTextureCount) continue;
            if (m_textures[texIdx] == INVALID_TEXTURE) continue;

            // MC SingleQuadParticle.extractRotatedQuad: lerp(xo → x), in
            // double, then the view's render origin comes off before the
            // narrowing to float (RenderOrigin.hpp) — MC subtracts the
            // camera position at exactly this point for the same reason.
            const glm::dvec3 interp(p.xo + (p.x - p.xo) * static_cast<double>(pt),
                                    p.yo + (p.y - p.yo) * static_cast<double>(pt),
                                    p.zo + (p.z - p.zo) * static_cast<double>(pt));
            const float size = QuadSizeFor(p, pt);
            if (size <= 0.0f) continue;

            // MC Particle.getLightCoords: the light at the particle's cell
            // (the full-bright kinds: block light 15 — the explosion's
            // FULL_BRIGHT sky 15 as well), times the lightmap. Every
            // particle of a bucket shares one draw, so each carries its own
            // light in its vertex colour (the draw's uEntityLight is 1).
            glm::vec3 light;
            {
                namespace LC = Game::Lighting::LightCoords;
                const bool explosion = texIdx >= kTexExplosion0 && texIdx < kTexExplosion0 + 16;
                int packed = explosion ? LC::kFullBright : EntityEnvironment::PackedLightAt(interp);
                if (!explosion && fullBright(p, texIdx)) packed = LC::WithBlock(packed, 15);
                light = EntityEnvironment::LightColor(packed);
            }
            const uint8_t r = static_cast<uint8_t>(std::clamp(p.rCol * light.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t g = static_cast<uint8_t>(std::clamp(p.gCol * light.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t b = static_cast<uint8_t>(std::clamp(p.bCol * light.b, 0.0f, 1.0f) * 255.0f);
            const uint8_t a = static_cast<uint8_t>(std::clamp(p.alpha, 0.0f, 1.0f) * 255.0f);

            const glm::vec3 center = Render::ToRender(interp);   // render space
            // MC SingleQuadParticle.render rotates the billboard about the
            // VIEW axis by `roll` when the particle has one. Only falling dust
            // does today, and it is what makes the dust tumble as it drifts
            // rather than sliding down flat.
            glm::vec3 right = camRight;
            glm::vec3 up    = camUp;
            if (p.rollSpeed != 0.0f) {
                const float roll = p.oRoll + (p.roll - p.oRoll) * pt;
                const float cs = std::cos(roll);
                const float sn = std::sin(roll);
                right = camRight * cs + camUp * sn;
                up    = camUp * cs - camRight * sn;
            }
            right *= size;
            up    *= size;
            // MC's quad: corner (+1,·) takes u0 and (-1,·) takes u1, and
            // MC's camera basis maps local +X to the camera's LEFT
            // (Camera.left = rotation * (1,0,0)) — so the sprite's u0 edge
            // is on the viewer's left, same as here with `right` = camera
            // right. v0 is the top row. The rectangle is the whole texture
            // for the one-file sprites and an atlas cell for the block
            // marker.
            const glm::vec3 c0 = center - right - up;   // uv (u0,v1)
            const glm::vec3 c1 = center + right - up;   // uv (u1,v1)
            const glm::vec3 c2 = center + right + up;   // uv (u1,v0)
            const glm::vec3 c3 = center - right + up;   // uv (u0,v0)

            auto& verts = buckets[texIdx * 2 + (fullBright(p, texIdx) ? 1 : 0)];
            verts.push_back({c0.x, c0.y, c0.z, p.u0, p.v1, r, g, b, a});
            verts.push_back({c1.x, c1.y, c1.z, p.u1, p.v1, r, g, b, a});
            verts.push_back({c2.x, c2.y, c2.z, p.u1, p.v0, r, g, b, a});
            verts.push_back({c0.x, c0.y, c0.z, p.u0, p.v1, r, g, b, a});
            verts.push_back({c2.x, c2.y, c2.z, p.u1, p.v0, r, g, b, a});
            verts.push_back({c3.x, c3.y, c3.z, p.u0, p.v0, r, g, b, a});
        }

        size_t totalVerts = 0;
        for (const auto& b : buckets) totalVerts += b.size();
        if (totalVerts == 0) return;

        // One upload for the whole frame; per-texture draws address ranges
        // via firstVertex. (Never re-upload between draws — the Vulkan
        // backend records the copies immediately but executes the draws at
        // submit, so a second upload would clobber the first. Same pitfall
        // PortalParticleSystem documents.)
        // This call's own buffer (see StreamSlot), grown if it must be.
        StreamSlot& slot = AcquireSlot(totalVerts, 4096);
        size_t offset = 0;
        for (const auto& b : buckets) {
            if (b.empty()) continue;
            g_renderBackend->UpdateBuffer(slot.vb, offset * 24,
                                          b.size() * 24, b.data());
            offset += b.size();
        }

        g_renderBackend->BindShader(m_shader);
        const glm::mat4 mvp = projection * view;
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformInt(m_shader, "uSprite", 0);
        // The frame's fog, measured from this view's eye (render space).
        EntityEnvironment::ApplyWorld(m_shader, glm::dvec3(cameraPos));
        // Each particle's light is in its vertex colour (see the build
        // above); the draw's own is 1.
        EntityEnvironment::SetEntityLight(m_shader, glm::vec3(1.0f));

        // MC SingleQuadParticle.Layer — the particle engine draws OPAQUE and
        // TRANSLUCENT as two separate passes with different pipeline state,
        // and nearly every particle is OPAQUE (heart, poof, smoke, explode,
        // huge explosion, falling dust, the geyser); only the spell swirl and
        // the noxious gas are translucent.
        //
        // The OPAQUE pass writes depth and does NOT blend: the 0.1 alpha
        // cutout in the shader is what shapes the sprite, so blending buys
        // nothing and costs correct occlusion — falling dust drawn blended
        // and depth-write-off showed through the block it was falling off.
        //
        // Two passes over the same vertex buffer, addressed by firstVertex,
        // so this adds no upload and at most one extra draw per texture.
        const auto drawPass = [&](bool opaquePass) {
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthWriteEnabled = opaquePass;
            s.colorWriteEnabled = true;
            s.blendEnabled      = !opaquePass;
            s.srcBlendFactor    = BlendFactor::SrcAlpha;
            s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            g_renderBackend->SetPipelineState(s);

            uint32_t first = 0;
            for (int bi = 0; bi < kTextureCount * 2; ++bi) {
                const auto& b = buckets[bi];
                if (b.empty()) continue;
                const int t = bi / 2;
                // Buckets are per TEXTURE, and the spell and noxious gas
                // sheets are the only translucent ones — so the layer split
                // falls out of the texture split with no extra bucketing.
                if (IsTranslucentTexture(t) != !opaquePass) {
                    first += static_cast<uint32_t>(b.size());
                    continue;
                }
                g_renderBackend->BindTexture(m_textures[t], 0);
                g_renderBackend->DrawArrays(slot.mesh,
                                            static_cast<uint32_t>(b.size()),
                                            /*firstVertex=*/first);
                first += static_cast<uint32_t>(b.size());
            }
        };
        drawPass(/*opaquePass=*/true);
        drawPass(/*opaquePass=*/false);
        g_renderBackend->UnbindMesh();

        // Restore the default pipeline.
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

} // namespace Render
