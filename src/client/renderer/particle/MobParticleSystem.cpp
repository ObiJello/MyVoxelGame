// File: src/client/renderer/particle/MobParticleSystem.cpp
//
// The engine half: resources, the spawn gates, the groups, the 20 Hz loop and
// the renderer. The providers (every type's constructor) are
// ParticleProviders.cpp; the per-type tick / size / light / sprite rules are
// ParticleTicks.cpp. See the header for scope and the deviations.

#include "MobParticleSystem.hpp"
#include "common/core/Features.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../mesh/ChunkRenderer.hpp"   // PortalEntityClipPlane
#include "../environment/EntityEnvironment.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/world/lighting/LightCoords.hpp"
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

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>

namespace Render {

    MobParticleSystem g_mobParticleSystem;

    // Shader — textured quad × per-vertex colour (the particle's colour times
    // its lightmap colour), MC's 0.1 alpha cutout, then the terrain's fog.
    // Vertex format is the standard 24-byte block layout so the default mesh
    // path handles it on both backends (Vulkan: shaders/mob_particle_vk.*).
    const char* MobParticleSystem::s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;    // render-space corner
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;  // particle rCol/gCol/bCol/alpha x lightmap

uniform mat4 uMVP;
// A portal view's clip plane in render space (ChunkRenderer::
// PortalEntityClipPlane / PortalClipPlane); zero = no clipping.
uniform vec4 uEntityClipPlane;

out vec2 vUV;
out vec4 vColor;
out vec3 vRenderPos;   // for the fog

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vRenderPos = aPos;
    gl_ClipDistance[0] = (any(notEqual(uEntityClipPlane.xyz, vec3(0.0))))
        ? dot(uEntityClipPlane.xyz, aPos) + uEntityClipPlane.w
        : 1.0;
}
)";

    const char* MobParticleSystem::s_fragSource = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec3 vRenderPos;
#ifndef OIT_ALPHA_ONLY
out vec4 FragColor;
#endif

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

// Improved Transparency (MC 26.3 OIT): the OIT variants are this source with
// #define OIT and a stage define; the GL backend splices shaders/oit_lib.glsl
// in here. The engine's own compile never sees any of it.
#ifdef OIT
#pragma oit_library
#endif

void main() {
    vec4 s = texture(uSprite, vUV);
    vec4 c = s * vColor;
    // MC's PARTICLE cutout threshold (alpha test 0.1).
    if (c.a < 0.1) discard;
#ifdef OIT_ALPHA_ONLY
    executeAlphaOnlyPhase(gl_FragCoord.z, c.a);   // MC particle.fsh
#else
    c.rgb *= uEntityLight;
    vec3 fogDelta = vRenderPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    c.rgb = mix(c.rgb, uFogColor.rgb, fogValue * uFogColor.a);
    FragColor = c;
#ifdef OIT_ACCUMULATE
    FragColor = sampleColorForAccumulation(FragColor);   // MC calculateFinalColor
#endif
#endif
}
)";

    MobParticleSystem::MobParticleSystem()
        : m_rng(std::chrono::steady_clock::now().time_since_epoch().count())
        , m_limiterRng(std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x5DEECE66DLL) {
    }

    MobParticleSystem::~MobParticleSystem() {
        Shutdown();
    }

    void MobParticleSystem::Shutdown() {
        if (!g_renderBackend) return;
        DestroySlots();
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_sprites.Destroy();
        if (m_elderGuardianTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_elderGuardianTexture);
            m_elderGuardianTexture = INVALID_TEXTURE;
        }
        m_particles.clear();
        m_incoming.clear();
        m_setByKind.clear();
        m_groupCount[0] = m_groupCount[1] = m_groupCount[2] = 0;
        m_sporeBlossomCount = 0;
    }

    void MobParticleSystem::ReloadTextures() {
        if (!g_renderBackend || m_shader == INVALID_SHADER) return;
        // MC ParticleResources.reload rebinds every sprite set; particles
        // alive across it keep their SET and pick from the new sprites. A
        // particle holding a fixed sprite id re-resolves below.
        m_sprites.Load();
        m_setByKind.clear();
        for (Particle& p : m_particles) {
            if (p.sheet != Sheet::Particles || p.walk) continue;
            if (p.spriteSet >= 0) p.sprite = m_sprites.Random(p.spriteSet, m_rng);
            else if (!m_sprites.Valid(p.sprite)) p.sprite = -1;
        }
    }

    bool MobParticleSystem::Initialize() {
        if (!g_renderBackend) return false;

        if (g_renderBackend->GetType() == BackendType::OpenGL) {
            m_shader = g_renderBackend->CreateShader(s_vertSource, s_fragSource);
        } else {
            // Vulkan: precompiled SPIR-V (shaders/mob_particle_vk.*.spv) on
            // the portal pipeline layout — the fragment shader reads the
            // frame's fog from the Common UBO (EntityEnvironment.hpp).
            m_shader = EntityEnvironment::CreateShader(
                "shaders/mob_particle.vert", "shaders/mob_particle.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[MobParticleSystem] Failed to load shader for backend %s",
                         g_renderBackend->GetName());
            return false;
        }
        if (!m_sprites.Load()) {
            Log::Warning("[MobParticleSystem] no particle sprites loaded — particles will not draw");
        }
        // The streaming buffers are made on first use (AcquireSlot).
        return true;
    }

    MobParticleSystem::StreamSlot& MobParticleSystem::AcquireSlot(size_t vertsNeeded, size_t minCapacity) {
        // This frame's set (see StreamSlot): a fresh slot per call, the set
        // growing to the frame's call count.
        std::vector<StreamSlot>& set = m_frameSlots[m_frameSet];
        if (m_slotCursor >= kMaxSlotsPerFrame) m_slotCursor = 0;
        if (m_slotCursor >= set.size()) set.resize(m_slotCursor + 1);
        StreamSlot& slot = set[m_slotCursor++];
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
        for (std::vector<StreamSlot>& set : m_frameSlots) {
            for (StreamSlot& slot : set) {
                if (slot.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(slot.mesh);  slot.mesh = INVALID_MESH; }
                if (slot.vb   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(slot.vb); slot.vb = INVALID_BUFFER; }
                slot.capacityVerts = 0;
            }
            set.clear();
        }
        m_slotCursor = 0;
    }

    // ── The spawn gates (ClientLevel.doAddParticle) ────────────────────────

    bool MobParticleSystem::OverridesParticleLimiter(Game::ParticleKind kind) {
        return Game::ParticleTypes::OverrideLimiter(kind);
    }

    // The kinds this engine only spawns through addAlwaysVisibleParticle:
    // the explosion emitter (LevelEventHandler 3000, the explode packet),
    // PotentSulfurBlock.animateTick's SULFUR_BUBBLES and
    // NoxiousGasCloudParticle's NOXIOUS_GAS. Their requests predate the
    // queue's alwaysShow flag, so the kind answers for them.
    bool MobParticleSystem::AlwaysShown(Game::ParticleKind kind) {
        return kind == Game::ParticleKind::ExplosionEmitter ||
               kind == Game::ParticleKind::SulfurBubbles ||
               kind == Game::ParticleKind::NoxiousGas;
    }

    bool MobParticleSystem::ShouldSpawn(const Client::ClientLevelBridge::QueuedParticle& q,
                                        const glm::vec3& cameraPos) {
        using Game::ParticleStatus;
        // MC addParticle: particle.getType().getOverrideLimiter() || overrideLimiter.
        const bool overrideLimiter = q.overrideLimiter || OverridesParticleLimiter(q.kind);
        const bool alwaysShow = q.alwaysShow || AlwaysShown(q.kind);

        // calculateParticleLevel — rolled before the limiter test, as MC does.
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

    void MobParticleSystem::SpawnFromRequest(const Client::ClientLevelBridge::QueuedParticle& q,
                                             Game::DimensionId dimension) {
        // The request's options: the full record when it came through
        // DoAddParticle, else rebuilt from the legacy fields (the colour of
        // AddColorParticle, the block marker's state).
        Game::ParticleOptions options;
        if (q.hasOptions) {
            options = q.options;
            options.kind = q.kind;
        } else {
            options.kind = q.kind;
            options.r = q.r; options.g = q.g; options.b = q.b; options.a = q.a;
            options.blockState = q.blockState;
        }

        const Game::IBlockAccess* blocks = nullptr;
        if (Client::ClientLevels::HasSession()) {
            if (Client::ClientLevel* level = Client::ClientLevels::Get(dimension)) blocks = level->Blocks();
        }

        Particle p;
        p.dimension = dimension;
        if (!MakeParticle(p, q, options, blocks)) return;   // the provider returned null
        // The modifiers a caller applied to the constructed particle (MC
        // ClientLevel.addBreakingParticles: .setPower(0.2F).scale(0.6F)).
        if (q.power != 1.0f) SetPower(p, q.power);
        if (q.scale != 1.0f) Scale(p, q.scale);
        AddParticle(std::move(p));
    }

    void MobParticleSystem::AddParticle(Particle&& p) {
        // MC ParticleEngine.add: a limited particle only while its limit has
        // room (ParticleLimit.SPORE_BLOSSOM: 1000).
        if (p.limitedSporeBlossom && m_sporeBlossomCount >= 1000) return;

        // MC ParticleGroup.add: a hard cap of 16384 per group, and between
        // 12288 and the cap a probabilistic reservoir — accepted with
        // probability (free / 4096)^2, so a flood thins out smoothly
        // instead of starving later spawns outright.
        const size_t group = static_cast<size_t>(p.group);
        const size_t size = m_groupCount[group];
        if (size >= 16384) return;
        if (size >= 12288) {
            const float freeSpace = static_cast<float>(16384 - size) / 4096.0f;
            if (m_rng.NextFloat() >= freeSpace * freeSpace) return;
        }
        ++m_groupCount[group];
        if (p.limitedSporeBlossom) ++m_sporeBlossomCount;
        m_particles.push_back(std::move(p));
    }

    // ── Simulation — MC Particle.move at fixed 20 Hz ────────────────────────

    void MobParticleSystem::SetSize(Particle& p, float w, float h) {
        // The box is centred on (x, z) with its floor at y, so moving the
        // particle keeps x/z/y where they are — only the extents change.
        p.bbWidth = w;
        p.bbHeight = h;
    }

    void MobParticleSystem::Scale(Particle& p, float s) {
        // SingleQuadParticle.scale: quadSize *= s, then Particle.scale's
        // setSize(0.2 * s, 0.2 * s).
        p.quadSize *= s;
        SetSize(p, 0.2f * s, 0.2f * s);
    }

    void MobParticleSystem::SetPower(Particle& p, float power) {
        p.xd *= static_cast<double>(power);
        p.yd = (p.yd - 0.10000000149011612) * static_cast<double>(power) + 0.10000000149011612;
        p.zd *= static_cast<double>(power);
    }

    void MobParticleSystem::MoveParticle(Particle& p, double xa, double ya, double za,
                                         const Game::IBlockAccess* blocks) {
        // The overrides that are a plain bounding-box shift (FlameParticle,
        // EndRodParticle, PortalParticle, SuspendedTownParticle, …): no
        // collision, no stoppedByCollision, no onGround.
        if (p.freeMove) {
            p.x += xa; p.y += ya; p.z += za;
            return;
        }

        // MC Particle.move, verbatim structure.
        if (p.stoppedByCollision) return;

        const double origXa = xa, origYa = ya, origZa = za;
        // MC MAXIMUM_COLLISION_VELOCITY_SQUARED = 100².
        if (p.hasPhysics && blocks != nullptr &&
            (xa != 0.0 || ya != 0.0 || za != 0.0) &&
            xa * xa + ya * ya + za * za < 10000.0) {
            const double hw = static_cast<double>(p.bbWidth) * 0.5;
            Game::AABBd box;
            box.min = glm::dvec3(p.x - hw, p.y, p.z - hw);
            box.max = glm::dvec3(p.x + hw, p.y + static_cast<double>(p.bbHeight), p.z + hw);

            Game::AABBd region = box;
            region.min += glm::dvec3(std::min(xa, 0.0), std::min(ya, 0.0), std::min(za, 0.0));
            region.max += glm::dvec3(std::max(xa, 0.0), std::max(ya, 0.0), std::max(za, 0.0));

            Game::PhysicsContext ctx;
            ctx.blockAccess = blocks;
            // Its OWN buffer, not the thread_local one MoveEntity uses — both
            // run on the client main thread, so sharing would alias.
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

        if (std::abs(origYa) >= 9.999999747378752E-6 && std::abs(ya) < 9.999999747378752E-6) {
            p.stoppedByCollision = true;
        }
        p.onGround = origYa != ya && origYa < 0.0;
        if (origXa != xa) p.xd = 0.0;
        if (origZa != za) p.zd = 0.0;
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

    MobParticleSystem::ChildSpawn MobParticleSystem::Child(Game::ParticleKind kind, double x, double y,
                                                           double z, double xd, double yd, double zd) {
        ChildSpawn c;
        c.q.kind = kind;
        c.q.x = x; c.q.y = y; c.q.z = z;
        c.q.vx = xd; c.q.vy = yd; c.q.vz = zd;
        return c;
    }

    void MobParticleSystem::Update(float dt, const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("MobParticles.Update");
        if (m_shader == INVALID_SHADER) return;
        m_camera = glm::dvec3(cameraPos);

        // 1. Drain queued spawn requests (main thread — see the queue note
        //    in ClientLevelBridge), from EVERY level the client holds: the
        //    one the player stands in and the far sides it sees through
        //    immersive portals. Each particle is stamped with its level.
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
        std::vector<ChildSpawn> spawns;
        std::vector<Game::DimensionId> spawnDims;
        while (m_tickAccum >= kTick && steps < 5) {
            m_tickAccum -= kTick;
            ++steps;
            ++m_ticks;
            spawns.clear();
            spawnDims.clear();
            // Indexed: a tick never appends to m_particles (spawns wait in
            // `spawns`), so the references stay valid.
            for (size_t i = 0; i < m_particles.size(); ++i) {
                Particle& p = m_particles[i];
                if (p.removed) continue;
                const size_t before = spawns.size();
                TickParticle(p, blocksOf(p.dimension), spawns);
                for (size_t s = before; s < spawns.size(); ++s) spawnDims.push_back(p.dimension);
            }
            // MC ParticleGroup.tickParticles: the dead leave, and their
            // group / limit counts with them.
            m_particles.erase(
                std::remove_if(m_particles.begin(), m_particles.end(), [this](const Particle& p) {
                    if (!p.removed) return false;
                    --m_groupCount[static_cast<size_t>(p.group)];
                    if (p.limitedSporeBlossom) --m_sporeBlossomCount;
                    return true;
                }),
                m_particles.end());
            // The children join AFTER the sweep — they must not be ticked (or
            // erased) in the tick that spawned them (MC's particlesToAdd).
            // Those spawned through level.addParticle face the same gates as
            // every other spawn; engine.add / engine.createParticle ones
            // (the firework's sparks and flash) go straight in.
            for (size_t i = 0; i < spawns.size(); ++i) {
                const Game::DimensionId dim = spawnDims[i];
                if (spawns[i].direct) {
                    Particle child = std::move(spawns[i].particle);
                    child.dimension = dim;
                    AddParticle(std::move(child));
                    continue;
                }
                if (!ShouldSpawn(spawns[i].q, AnchorFor(dim, cameraPos))) continue;
                SpawnFromRequest(spawns[i].q, dim);
            }
        }
        if (steps == 5) m_tickAccum = 0.0f;
        m_partialTick = std::clamp(m_tickAccum / kTick, 0.0f, 1.0f);
    }

    std::string MobParticleSystem::DebugCounts() const {
        // MC ParticleEngine.countParticles, in RENDER_ORDER plus NO_RENDER.
        const size_t sq = m_groupCount[static_cast<size_t>(Group::SingleQuads)];
        const size_t nr = m_groupCount[static_cast<size_t>(Group::NoRender)];
        const size_t eg = m_groupCount[static_cast<size_t>(Group::ElderGuardians)];
        return "SQ " + std::to_string(sq) + " NR " + std::to_string(nr) + " EG " + std::to_string(eg) +
               " T " + std::to_string(sq + nr + eg);
    }

    // ── Rendering ──────────────────────────────────────────────────────────

    namespace {

        // A light-coords value (packed or smooth — they share bit positions)
        // through the frame's lightmap. MC samples the 16x16 lightmap at
        // (block, sky) / 16 with linear filtering, so a smooth value between
        // two levels (addSmoothBlockEmission's ramp) blends the two texels.
        glm::vec3 LightmapColor(int coords) {
            namespace LC = Game::Lighting::LightCoords;
            const float block = std::clamp(static_cast<float>(LC::SmoothBlock(coords)) / 16.0f, 0.0f, 15.0f);
            const float sky = std::clamp(static_cast<float>(LC::SmoothSky(coords)) / 16.0f, 0.0f, 15.0f);
            const int b0 = static_cast<int>(block), s0 = static_cast<int>(sky);
            const int b1 = std::min(b0 + 1, 15), s1 = std::min(s0 + 1, 15);
            const float fb = block - static_cast<float>(b0), fs = sky - static_cast<float>(s0);
            const glm::vec3 c00 = EntityEnvironment::LightColor(LC::Pack(b0, s0));
            if (fb == 0.0f && fs == 0.0f) return c00;
            const glm::vec3 c10 = EntityEnvironment::LightColor(LC::Pack(b1, s0));
            const glm::vec3 c01 = EntityEnvironment::LightColor(LC::Pack(b0, s1));
            const glm::vec3 c11 = EntityEnvironment::LightColor(LC::Pack(b1, s1));
            return glm::mix(glm::mix(c00, c10, fb), glm::mix(c01, c11, fb), fs);
        }

        struct Vert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };

    } // namespace

    void MobParticleSystem::Render(const glm::mat4& projection,
                                   const glm::mat4& view,
                                   const glm::vec3& cameraPos,
                                   Game::DimensionId dimension) {
        PROFILE_ZONE_N("MobParticles.Render");
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        if (m_particles.empty()) return;

        // The sheets. The block and item atlases are fetched per draw because
        // a resource-pack reload rebuilds them under new handles.
        TextureHandle sheets[kSheetCount] = {
            m_sprites.Texture(),
            GetAtlasTexture(AtlasId::Blocks),
            GetAtlasTexture(AtlasId::Items),
            INVALID_TEXTURE,
        };

        // Camera basis (MC Camera: local +X is the camera's right, +Y up).
        const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);
        // LOOKAT_Y: the camera's yaw only (the rotation's y/w components) —
        // a horizontal right axis and world up.
        glm::vec3 yawRight(camRight.x, 0.0f, camRight.z);
        {
            const float len = glm::length(yawRight);
            yawRight = len > 1e-6f ? yawRight / len : glm::vec3(1.0f, 0.0f, 0.0f);
        }

        // Buckets per (sheet, layer): [sheet * 2 + translucent].
        static thread_local std::vector<Vert> buckets[kSheetCount * 2];
        for (auto& b : buckets) b.clear();

        const float pt = m_partialTick;

        const auto emitQuad = [&](std::vector<Vert>& verts, const glm::vec3& center, glm::vec3 right, glm::vec3 up,
                                  float size, float qu0, float qv0, float qu1, float qv1,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
            // MC QuadParticleRenderState.renderRotatedQuad: local (1,-1) takes
            // (u1, v1), (1, 1) (u1, v0), (-1, 1) (u0, v0), (-1,-1) (u0, v1).
            right *= size;
            up    *= size;
            const glm::vec3 c0 = center - right - up;   // (u0, v1)
            const glm::vec3 c1 = center + right - up;   // (u1, v1)
            const glm::vec3 c2 = center + right + up;   // (u1, v0)
            const glm::vec3 c3 = center - right + up;   // (u0, v0)
            verts.push_back({c0.x, c0.y, c0.z, qu0, qv1, r, g, b, a});
            verts.push_back({c1.x, c1.y, c1.z, qu1, qv1, r, g, b, a});
            verts.push_back({c2.x, c2.y, c2.z, qu1, qv0, r, g, b, a});
            verts.push_back({c0.x, c0.y, c0.z, qu0, qv1, r, g, b, a});
            verts.push_back({c2.x, c2.y, c2.z, qu1, qv0, r, g, b, a});
            verts.push_back({c3.x, c3.y, c3.z, qu0, qv0, r, g, b, a});
        };

        for (const auto& p : m_particles) {
            if (p.dimension != dimension || p.group != Group::SingleQuads || p.removed) continue;

            // The quad's texture rectangle.
            float qu0, qv0, qu1, qv1;
            if (p.sheet == Sheet::Particles) {
                const int sprite = SpriteFor(p);
                if (!m_sprites.Valid(sprite)) continue;
                const ParticleSpriteAtlas::Rect rect = m_sprites.FrameRect(sprite, m_ticks);
                if (p.subRect) {
                    qu0 = rect.u0 + (rect.u1 - rect.u0) * p.u0;
                    qu1 = rect.u0 + (rect.u1 - rect.u0) * p.u1;
                    qv0 = rect.v0 + (rect.v1 - rect.v0) * p.v0;
                    qv1 = rect.v0 + (rect.v1 - rect.v0) * p.v1;
                } else {
                    qu0 = rect.u0; qv0 = rect.v0; qu1 = rect.u1; qv1 = rect.v1;
                }
            } else {
                qu0 = p.u0; qv0 = p.v0; qu1 = p.u1; qv1 = p.v1;
            }
            const int sheetIndex = static_cast<int>(p.sheet);
            if (sheets[sheetIndex] == INVALID_TEXTURE) continue;

            // Per-type extract() overrides that change what is drawn.
            float alpha = p.alpha;
            float rCol = p.rCol, gCol = p.gCol, bCol = p.bCol;
            switch (p.kind) {
                case Game::ParticleKind::Firework:
                    // SparkParticle.extract: a twinkling spark draws on
                    // alternate thirds of its second two-thirds.
                    if (p.twinkle && !(p.age < p.lifetime / 3 || (p.age + p.lifetime) / 3 % 2 == 0)) continue;
                    break;
                case Game::ParticleKind::Shriek:
                    if (p.delay > 0) continue;
                    alpha = 1.0f - std::clamp((static_cast<float>(p.age) + pt) / static_cast<float>(p.lifetime), 0.0f, 1.0f);
                    break;
                case Game::ParticleKind::Flash:
                    // OverlayParticle.extract.
                    alpha = 0.6f - (static_cast<float>(p.age) + pt - 1.0f) * 0.25f * 0.5f;
                    break;
                case Game::ParticleKind::DustColorTransition: {
                    // DustColorTransitionParticle.lerpColors.
                    const float a = (static_cast<float>(p.age) + pt) / (static_cast<float>(p.lifetime) + 1.0f);
                    const glm::vec3 c = glm::mix(p.fromColor, p.toColor, a);
                    rCol = c.r; gCol = c.g; bCol = c.b;
                    break;
                }
                case Game::ParticleKind::Enchant:
                case Game::ParticleKind::Nautilus:
                case Game::ParticleKind::VaultConnection:
                    // FlyTowardsPositionParticle.extract: LifetimeAlpha.
                    if (p.laStart != p.laEnd) {
                        const float t = (static_cast<float>(p.age) + pt) / static_cast<float>(p.lifetime);
                        const float n = (t - p.laStartAt) / (p.laEndAt - p.laStartAt);
                        alpha = p.laStart + std::clamp(n, 0.0f, 1.0f) * (p.laEnd - p.laStart);
                    } else {
                        alpha = p.laStart;
                    }
                    break;
                default:
                    break;
            }

            const float size = QuadSizeFor(p, pt);
            if (!(size > 0.0f)) continue;

            // MC SingleQuadParticle.extractRotatedQuad: lerp(xo → x) in
            // double, then the view's render origin comes off before the
            // narrowing to float (RenderOrigin.hpp).
            const glm::dvec3 interp(p.xo + (p.x - p.xo) * static_cast<double>(pt),
                                    p.yo + (p.y - p.yo) * static_cast<double>(pt),
                                    p.zo + (p.z - p.zo) * static_cast<double>(pt));
            const glm::vec3 center = Render::ToRender(interp);

            const glm::vec3 light = LightmapColor(LightCoordsFor(p, pt));
            const uint8_t r = static_cast<uint8_t>(std::clamp(rCol * light.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t g = static_cast<uint8_t>(std::clamp(gCol * light.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t b = static_cast<uint8_t>(std::clamp(bCol * light.b, 0.0f, 1.0f) * 255.0f);
            const uint8_t a = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);

            auto& verts = buckets[sheetIndex * 2 + (TranslucentFor(p) ? 1 : 0)];

            switch (p.facing) {
                case Facing::LookAtXYZ:
                case Facing::LookAtY: {
                    glm::vec3 right = p.facing == Facing::LookAtY ? yawRight : camRight;
                    glm::vec3 up    = p.facing == Facing::LookAtY ? glm::vec3(0.0f, 1.0f, 0.0f) : camUp;
                    // rotation.rotateZ(lerp(oRoll, roll)) — about the view axis.
                    const float roll = p.oRoll + (p.roll - p.oRoll) * pt;
                    if (roll != 0.0f) {
                        const float cs = std::cos(roll);
                        const float sn = std::sin(roll);
                        const glm::vec3 r0 = right, u0 = up;
                        right = r0 * cs + u0 * sn;
                        up    = u0 * cs - r0 * sn;
                    }
                    emitQuad(verts, center, right, up, size, qu0, qv0, qu1, qv1, r, g, b, a);
                    break;
                }
                case Facing::Shriek: {
                    // ShriekParticle.extract: two world-fixed quads,
                    // rotationX(-1.0472) and rotationYXZ(-PI, 1.0472, 0).
                    const glm::quat q1 = glm::angleAxis(-1.0472f, glm::vec3(1, 0, 0));
                    const glm::quat q2 = glm::angleAxis(-3.1415927f, glm::vec3(0, 1, 0)) *
                                         glm::angleAxis(1.0472f, glm::vec3(1, 0, 0));
                    emitQuad(verts, center, q1 * glm::vec3(1, 0, 0), q1 * glm::vec3(0, 1, 0), size,
                             qu0, qv0, qu1, qv1, r, g, b, a);
                    emitQuad(verts, center, q2 * glm::vec3(1, 0, 0), q2 * glm::vec3(0, 1, 0), size,
                             qu0, qv0, qu1, qv1, r, g, b, a);
                    break;
                }
                case Facing::Vibration: {
                    // VibrationSignalParticle.extract: aimed along the flight,
                    // swaying, drawn from both sides.
                    const float sway = std::sin((static_cast<float>(p.age) + pt - 6.2831855f) * 0.05f) * 2.0f;
                    const float rot = p.rotO + (p.rot - p.rotO) * pt;
                    const float pitch = p.pitchO + (p.pitch - p.pitchO) * pt + 1.5707964f;
                    const glm::quat q1 = glm::angleAxis(rot, glm::vec3(0, 1, 0)) *
                                         glm::angleAxis(-pitch, glm::vec3(1, 0, 0)) *
                                         glm::angleAxis(sway, glm::vec3(0, 1, 0));
                    const glm::quat q2 = glm::angleAxis(-3.1415927f + rot, glm::vec3(0, 1, 0)) *
                                         glm::angleAxis(pitch, glm::vec3(1, 0, 0)) *
                                         glm::angleAxis(sway, glm::vec3(0, 1, 0));
                    emitQuad(verts, center, q1 * glm::vec3(1, 0, 0), q1 * glm::vec3(0, 1, 0), size,
                             qu0, qv0, qu1, qv1, r, g, b, a);
                    emitQuad(verts, center, q2 * glm::vec3(1, 0, 0), q2 * glm::vec3(0, 1, 0), size,
                             qu0, qv0, qu1, qv1, r, g, b, a);
                    break;
                }
            }
        }

        size_t totalVerts = 0;
        for (const auto& b : buckets) totalVerts += b.size();
        if (totalVerts == 0) return;

        // One upload for the whole call; per-bucket draws address ranges via
        // firstVertex (never re-upload between draws — the Vulkan backend
        // records the copies immediately but executes the draws at submit).
        StreamSlot& slot = AcquireSlot(totalVerts, 4096);
        {
            PROFILE_ZONE_N("MobParticles.Upload");
            size_t offset = 0;
            for (const auto& b : buckets) {
                if (b.empty()) continue;
                g_renderBackend->UpdateBufferStreaming(slot.vb, offset * 24, b.size() * 24, b.data());
                offset += b.size();
            }
        }

        {
            PROFILE_ZONE_N("MobParticles.Setup");
            g_renderBackend->BindShader(m_shader);
            const glm::mat4 mvp = projection * view;
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->SetUniformInt(m_shader, "uSprite", 0);
            // The frame's fog, measured from this view's eye (render space).
            EntityEnvironment::ApplyWorld(m_shader, glm::dvec3(cameraPos));
            // Each particle's light is in its vertex colour; the draw's own is 1.
            EntityEnvironment::SetEntityLight(m_shader, glm::vec3(1.0f));
            // The view's portal clip plane (zero in the main view) — set
            // every draw: on Vulkan it rides a push-constant slot the entity
            // draws also write.
            g_renderBackend->SetUniformVec4(m_shader, "uEntityClipPlane", ChunkRenderer::PortalEntityClipPlane());
        }

        // MC's two particle pipelines: OPAQUE_PARTICLE (depth write, no
        // blend, the cutout shapes the sprite) then TRANSLUCENT_PARTICLE
        // (blended, no depth write).
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
            for (int bi = 0; bi < kSheetCount * 2; ++bi) {
                const auto& b = buckets[bi];
                if (b.empty()) continue;
                const bool translucent = (bi & 1) != 0;
                if (translucent == opaquePass) {
                    first += static_cast<uint32_t>(b.size());
                    continue;
                }
                g_renderBackend->BindTexture(sheets[bi / 2], 0);
                g_renderBackend->DrawArrays(slot.mesh, static_cast<uint32_t>(b.size()), first);
                first += static_cast<uint32_t>(b.size());
            }
        };
        { PROFILE_ZONE_N("MobParticles.DrawOpaque");      drawPass(/*opaquePass=*/true); }
        { PROFILE_ZONE_N("MobParticles.DrawTranslucent"); drawPass(/*opaquePass=*/false); }
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
