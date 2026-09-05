// File: src/client/renderer/particle/MobParticleSystem.cpp
// See header for scope and the list of deliberate deviations.

#include "MobParticleSystem.hpp"
#include "common/core/Features.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/world/ClientBlockAccess.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#endif
#include "platform/GameDirectory.hpp"

#include "stb_image.h"
#include <algorithm>
#include <chrono>
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
layout(location = 0) in vec3 aPos;    // world-space corner
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;  // particle rCol/gCol/bCol/alpha

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

    const char* MobParticleSystem::s_fragSource = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D uSprite;

void main() {
    vec4 s = texture(uSprite, vUV);
    vec4 c = s * vColor;
    // MC's PARTICLE cutout threshold (alpha test 0.1) — applied to the
    // blended pass so fully transparent texels never write.
    if (c.a < 0.1) discard;
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
        for (auto& t : m_textures) {
            if (t != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(t); t = INVALID_TEXTURE; }
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
    }

    void MobParticleSystem::ReloadTextures() {
        if (!g_renderBackend || m_shader == INVALID_SHADER) return;
        for (TextureHandle& t : m_textures) {
            if (t != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(t); t = INVALID_TEXTURE; }
        }
        LoadSprites();
    }

    bool MobParticleSystem::Initialize() {
        if (!g_renderBackend) return false;

        if (g_renderBackend->GetType() == BackendType::OpenGL) {
            m_shader = g_renderBackend->CreateShader(s_vertSource, s_fragSource);
        } else {
            // Vulkan: precompiled SPIR-V (shaders/mob_particle_vk.*.spv),
            // same push-constant block and block vertex layout as the rest
            // of the VK pipeline — see PortalParticleSystem's note.
            m_shader = g_renderBackend->CreateShaderFromFiles(
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

    // MC ParticleType.getOverrideLimiter() — true only for the types spawned
    // through addAlwaysVisibleParticle. EXPLOSION_EMITTER is the one this
    // engine spawns that way (LevelEventHandler:340); a distant blast keeps
    // its fireball and loses only the debris.
    bool MobParticleSystem::OverridesParticleLimiter(Game::ParticleKind kind) {
        return kind == Game::ParticleKind::ExplosionEmitter;
    }

    bool MobParticleSystem::ShouldSpawn(const Client::ClientLevelBridge::QueuedParticle& q,
                                        const glm::vec3& cameraPos) {
        using Game::ParticleStatus;
        const bool overrideLimiter = OverridesParticleLimiter(q.kind);
        const bool alwaysShow = overrideLimiter;   // addAlwaysVisibleParticle

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
            // Particle AABB: setSize(0.2, 0.2) around (x, y feet, z).
            Game::AABBd box;
            box.min = glm::dvec3(p.x - 0.1, p.y, p.z - 0.1);
            box.max = glm::dvec3(p.x + 0.1, p.y + 0.2, p.z + 0.1);

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

        if (p.age++ >= p.lifetime) {
            p.removed = true;
            return;
        }

        if (p.kind == Game::ParticleKind::Explosion) {
            // MC HugeExplosionParticle.tick: ages, never moves.
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
            case Game::ParticleKind::ExplosionEmitter:
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
                // MC HeartParticle / BaseAshSmokeParticle / FallingDustParticle
                // all share getQuadSize: ramp in over the first 1/32 of the
                // lifetime, so a particle fades IN rather than popping.
                return p.quadSize *
                       std::clamp((static_cast<float>(p.age) + partialTick) /
                                      static_cast<float>(p.lifetime) * 32.0f,
                                  0.0f, 1.0f);
            default:
                return p.quadSize;
        }
    }

    void MobParticleSystem::Render(const glm::mat4& projection,
                                   const glm::mat4& view,
                                   const glm::vec3& cameraPos,
                                   Game::DimensionId dimension) {
        PROFILE_ZONE_N("MobParticles.Render");
        (void)cameraPos;
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        if (m_particles.empty()) return;

        struct Vert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };

        // Camera basis for billboarding (LOOKAT_XYZ — full camera facing).
        const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);

        // Bucket vertices per sprite texture so each texture is one draw.
        static thread_local std::vector<Vert> buckets[kTextureCount];
        for (auto& b : buckets) b.clear();

        const float pt = m_partialTick;
        for (const auto& p : m_particles) {
            if (p.dimension != dimension) continue;
            const int texIdx = TextureIndexFor(p);
            if (texIdx < 0 || texIdx >= kTextureCount) continue;
            if (m_textures[texIdx] == INVALID_TEXTURE) continue;

            // MC SingleQuadParticle.extractRotatedQuad: lerp(xo → x).
            const float px = static_cast<float>(
                p.xo + (p.x - p.xo) * static_cast<double>(pt));
            const float py = static_cast<float>(
                p.yo + (p.y - p.yo) * static_cast<double>(pt));
            const float pz = static_cast<float>(
                p.zo + (p.z - p.zo) * static_cast<double>(pt));
            const float size = QuadSizeFor(p, pt);
            if (size <= 0.0f) continue;

            const uint8_t r = static_cast<uint8_t>(std::clamp(p.rCol, 0.0f, 1.0f) * 255.0f);
            const uint8_t g = static_cast<uint8_t>(std::clamp(p.gCol, 0.0f, 1.0f) * 255.0f);
            const uint8_t b = static_cast<uint8_t>(std::clamp(p.bCol, 0.0f, 1.0f) * 255.0f);
            const uint8_t a = static_cast<uint8_t>(std::clamp(p.alpha, 0.0f, 1.0f) * 255.0f);

            const glm::vec3 center(px, py, pz);
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
            const glm::vec3 c0 = center - right - up;   // uv (0,1)
            const glm::vec3 c1 = center + right - up;   // uv (1,1)
            const glm::vec3 c2 = center + right + up;   // uv (1,0)
            const glm::vec3 c3 = center - right + up;   // uv (0,0)

            auto& verts = buckets[texIdx];
            verts.push_back({c0.x, c0.y, c0.z, 0.0f, 1.0f, r, g, b, a});
            verts.push_back({c1.x, c1.y, c1.z, 1.0f, 1.0f, r, g, b, a});
            verts.push_back({c2.x, c2.y, c2.z, 1.0f, 0.0f, r, g, b, a});
            verts.push_back({c0.x, c0.y, c0.z, 0.0f, 1.0f, r, g, b, a});
            verts.push_back({c2.x, c2.y, c2.z, 1.0f, 0.0f, r, g, b, a});
            verts.push_back({c3.x, c3.y, c3.z, 0.0f, 0.0f, r, g, b, a});
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

        // MC SingleQuadParticle.Layer — the particle engine draws OPAQUE and
        // TRANSLUCENT as two separate passes with different pipeline state,
        // and nearly every particle is OPAQUE (heart, poof, smoke, explode,
        // huge explosion, falling dust); only the spell swirl is translucent.
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
            for (int t = 0; t < kTextureCount; ++t) {
                const auto& b = buckets[t];
                if (b.empty()) continue;
                // Buckets are per TEXTURE, and the spell sheet is the only
                // translucent one — so the layer split falls out of the
                // texture split with no extra bucketing.
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
