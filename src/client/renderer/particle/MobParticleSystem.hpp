// File: src/client/renderer/particle/MobParticleSystem.hpp
//
// The mob/world particle system — MC's client particle engine reduced to the
// particle TYPES the ported entity code actually spawns (see
// Game::ParticleKind in EntityLevel.hpp): hearts, taming/ambient smoke, poof,
// the explosion flash + emitter, and the entity-effect/witch spell swirl.
//
// Simulation is a port of $MC/client/particle/Particle.java +
// SingleQuadParticle.java and the concrete types (HeartParticle,
// SmokeParticle/BaseAshSmokeParticle, LargeSmokeParticle, ExplodeParticle,
// HugeExplosionParticle, HugeExplosionSeedParticle, SpellParticle) — all
// constants verbatim, integrated at MC's fixed 20 Hz with sub-tick render
// interpolation (xo→x lerp by partial tick), including MC's per-particle
// block collision for the types with hasPhysics.
//
// Rendering follows PortalParticleSystem's pattern: CPU-built camera-facing
// billboards, one streaming VB rebuilt per frame, batched into one draw per
// bound sprite texture, in MC's two layer passes (SingleQuadParticle.Layer):
// OPAQUE first — depth-write on, no blending, shaped by the shader's 0.1
// alpha cutout — then TRANSLUCENT (the spell sheet alone) blended with depth
// write off. Everything else this engine spawns is OPAQUE in MC too.
//
// REMAINING DEVIATIONS from MC's renderer:
//   * NO per-particle world lighting. MC multiplies in the lightmap from
//     Particle.getLightCoords (HugeExplosionParticle overrides it to
//     fullbright, 15728880, and is the one type that should be bright). This
//     engine keeps no per-cell light data on the client at all — IBlockAccess
//     ::GetRawBrightness walks the column with GetBlock and answers SKY
//     exposure only, which is both wrong for block light and far too
//     expensive to call per particle per frame. So every particle renders
//     fullbright, and dust in a dark cave glows. This is the one thing here
//     that is blocked on a subsystem rather than on effort: it becomes a
//     couple of lines in the vertex build the day a client light engine
//     lands.
//   * no camera-distance sort within a frame among the translucent pass.
//
// Spawn requests arrive through ClientLevelBridge's queue (the client half
// of MC Level.addParticle) — drained here every Update. Main thread only.

#pragma once

#include "../backend/RenderTypes.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace Game { struct IBlockAccess; }

namespace Render {

    class MobParticleSystem {
    public:
        MobParticleSystem();
        ~MobParticleSystem();

        bool Initialize();
        void Shutdown();

        // Drain queued spawn requests from the mob manager and advance the
        // simulation. dt is real frame time; internally the particles step
        // at MC's fixed 20 Hz, with the remainder kept as the render
        // partial tick. `blocks` feeds the hasPhysics types' block
        // collision (null = no collision, particles fly through).
        //
        // Every client level's queue is drained (the active one and the
        // far sides seen through immersive portals); each particle remembers
        // its level and collides against that level's blocks. The distance
        // cull for a far level is measured from the camera's image through
        // the nearest portal that leads there.
        void Update(float dt, const glm::vec3& cameraPos);

        // Draw every live particle as a camera-facing billboard, one draw
        // call per sprite texture in use.
        // Only the particles of `dimension` — the main pass draws the active
        // level's, a portal view draws its far level's.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, Game::DimensionId dimension);

        size_t Count() const { return m_particles.size(); }

        // MC ClientLevel.doAddParticle's limiter: a particle further than 32
        // blocks from the camera is DROPPED unless its type overrides the
        // limit. Only EXPLOSION_EMITTER does (LevelEventHandler uses
        // addAlwaysVisibleParticle for it), which is why a distant TNT still
        // shows its fireball but not the debris cloud around it.
        static constexpr double kParticleCutoffSq = 1024.0;   // 32 blocks
        static bool OverridesParticleLimiter(Game::ParticleKind kind);

        // MC ClientLevel.doAddParticle, all three gates in MC's order:
        //
        //     status = calculateParticleLevel(alwaysShow)
        //     if (overrideLimiter)            create
        //     else if (distSq > 1024)         drop
        //     else if (status == MINIMAL)     drop
        //     else                            create
        //
        // where calculateParticleLevel (ClientLevel.java:687) reads the
        // Particles option and randomly downgrades it — DECREASED drops one
        // in three, and an always-show particle under MINIMAL gets a one-in-
        // ten reprieve to DECREASED. `alwaysShow` is the addAlwaysVisible-
        // Particle flag; the emitter is the only kind spawned that way.
        bool ShouldSpawn(const Client::ClientLevelBridge::QueuedParticle& q,
                         const glm::vec3& cameraPos);

    private:
        // Sprite textures, one file each — index into m_textures.
        //   [0] heart  [1] angry
        //   [2..9]   generic_0..7   (smoke / large smoke / poof)
        //   [10..25] explosion_0..15
        //   [26..33] spell_0..7     (entity effect / witch)
        static constexpr int kTexHeart     = 0;
        static constexpr int kTexAngry     = 1;
        static constexpr int kTexGeneric0  = 2;   // 8 frames
        static constexpr int kTexExplosion0 = 10; // 16 frames
        static constexpr int kTexSpell0    = 26;  // 8 frames
        static constexpr int kTextureCount = 34;

        // MC SingleQuadParticle.Layer.TRANSLUCENT — the spell sheet alone.
        // Every other particle this engine spawns is OPAQUE (heart, poof,
        // smoke, explode, huge explosion, falling dust), so the layer split
        // is exactly the spell/not-spell split of the texture buckets.
        static constexpr bool IsTranslucentTexture(int texIndex) {
            return texIndex >= kTexSpell0;
        }

        // MC ParticleEngine.MAX_PARTICLES_PER_TYPE is 16384 per type; a
        // single shared cap keeps the worst case bounded the same way.
        static constexpr size_t kMaxParticles = 16384;

        // Slots only the limiter-overriding kinds (the explosion emitter) may
        // take. 6 children per emitter per tick for 8 ticks is 48 each, so this
        // covers ~21 concurrent blasts' worth of fireball. It is only
        // sufficient because the emitter's children are now distance-culled
        // too — without that gate this number would be decorative.
        static constexpr size_t kReservedForOverriding = 1024;

        // One particle — MC Particle + SingleQuadParticle state, doubles
        // where MC uses doubles.
        struct Particle {
            Game::ParticleKind kind;
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            double x = 0, y = 0, z = 0;
            double xo = 0, yo = 0, zo = 0;
            double xd = 0, yd = 0, zd = 0;
            int   age = 0;
            int   lifetime = 0;
            float gravity = 0.0f;
            float friction = 0.98f;
            bool  speedUpWhenYMotionIsBlocked = false;
            bool  hasPhysics = true;
            bool  onGround = false;
            bool  stoppedByCollision = false;
            bool  removed = false;
            float quadSize = 0.1f;
            float rCol = 1.0f, gCol = 1.0f, bCol = 1.0f, alpha = 1.0f;

            // MC Particle.roll / oRoll — the billboard's spin about the view
            // axis. Only FallingDust uses it today (it tumbles as it falls);
            // every other type here leaves it at zero, which costs the vertex
            // builder one branch and nothing else.
            float roll = 0.0f, oRoll = 0.0f;
            float rollSpeed = 0.0f;
        };

        void SpawnFromRequest(
            const Client::ClientLevelBridge::QueuedParticle& q,
            Game::DimensionId dimension);
        // Where the 32-block spawn cull is measured from for a level: the
        // camera, or its image through the nearest portal into that level.
        static glm::vec3 AnchorFor(Game::DimensionId dimension, const glm::vec3& cameraPos);
        void TickParticle(Particle& p, const Game::IBlockAccess* blocks,
                          std::vector<Client::ClientLevelBridge::QueuedParticle>&
                              emitterSpawns);
        // MC Particle.move — per-axis block collision for hasPhysics types.
        void MoveParticle(Particle& p, double xa, double ya, double za,
                          const Game::IBlockAccess* blocks);

        // Sprite index for the particle's current age (MC
        // SingleQuadParticle.setSpriteFromAge over the type's sprite sheet,
        // honouring each sheet's JSON frame order). -1 = not renderable
        // (the emitter seed).
        int TextureIndexFor(const Particle& p) const;
        // MC SingleQuadParticle.getQuadSize(partialTick) with the per-type
        // overrides (heart/smoke ramp in over the first 1/32 of life).
        float QuadSizeFor(const Particle& p, float partialTick) const;

        std::vector<Particle> m_particles;
        std::vector<Client::ClientLevelBridge::QueuedParticle> m_incoming;
        Game::JavaRandom m_rng;
        // MC ClientLevel.random, which calculateParticleLevel draws from —
        // a different stream from the ParticleEngine's, so the limiter's
        // rolls do not perturb the particle constructors' sequence.
        Game::JavaRandom m_limiterRng;
        // The Particles option, read once per Update on the main thread.
        Game::ParticleStatus m_particleStatus = Game::ParticleStatus::All;

        float m_tickAccum = 0.0f;   // seconds toward the next 20 Hz step
        float m_partialTick = 0.0f; // 0..1 for render interpolation

        // GPU resources — one shader, a ring of streaming VBs (block vertex
        // layout: pos3f + uv2f + rgba8 = 24 bytes) with a mesh each.
        ShaderHandle  m_shader = INVALID_SHADER;
        TextureHandle m_textures[kTextureCount];

        // One streaming vertex buffer per Render CALL, not per frame. The
        // system draws once per view — the main one and every portal view —
        // and the Vulkan backend records an upload immediately but runs
        // the draws at submit: a second upload into the same buffer
        // clobbers what the first draw will read, and growing that buffer
        // destroys one a recorded draw still references — VK_ERROR_
        // DEVICE_LOST, the picture freezing while the game runs on. Calls
        // cycle through the ring; a slot grows on its own, deferred.
        struct StreamSlot {
            BufferHandle vb = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            size_t       capacityVerts = 0;
        };
        static constexpr size_t kStreamSlots = 8;
        std::array<StreamSlot, kStreamSlots> m_slots;
        size_t m_slotCursor = 0;
        StreamSlot& AcquireSlot(size_t vertsNeeded, size_t minCapacity);
        void DestroySlots();

        static const char* s_vertSource;
        static const char* s_fragSource;
    };

    extern MobParticleSystem g_mobParticleSystem;

} // namespace Render
