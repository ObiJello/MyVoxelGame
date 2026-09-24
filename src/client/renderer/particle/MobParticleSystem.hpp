// File: src/client/renderer/particle/MobParticleSystem.hpp
//
// The mob/world particle system — MC's client particle engine reduced to the
// particle TYPES the ported entity code actually spawns (see
// Game::ParticleKind in EntityLevel.hpp): hearts, taming/ambient smoke, poof,
// the explosion flash + emitter, the entity-effect/witch spell swirl, the
// block marker a creative player sees while holding a barrier or light, and
// the potent sulfur geyser's bubbles, noxious gas and plume.
//
// Simulation is a port of $MC/client/particle/Particle.java +
// SingleQuadParticle.java and the concrete types (HeartParticle,
// SmokeParticle/BaseAshSmokeParticle, LargeSmokeParticle, ExplodeParticle,
// HugeExplosionParticle, HugeExplosionSeedParticle, SpellParticle,
// PortalParticle — the Hush portal's motes, recoloured — and 26.3's
// SulfurBubbleParticle, NoxiousGasParticle, NoxiousGasCloudParticle,
// GeyserEruptionParticle, GeyserBaseParticle and GeyserPlumeParticle) — all
// constants verbatim, integrated at MC's fixed 20 Hz with sub-tick render
// interpolation (xo→x lerp by partial tick), including MC's per-particle
// block collision for the types with hasPhysics.
//
// Rendering follows PortalParticleSystem's pattern: CPU-built camera-facing
// billboards, one streaming VB rebuilt per frame, batched into one draw per
// bound sprite texture, in MC's two layer passes (SingleQuadParticle.Layer):
// OPAQUE first — depth-write on, no blending, shaped by the shader's 0.1
// alpha cutout — then TRANSLUCENT (the spell and noxious gas sheets) blended
// with depth write off. Everything else this engine spawns is OPAQUE in MC too.
//
// REMAINING DEVIATIONS from MC's renderer:
//   * NO per-cell world lighting. MC multiplies in the lightmap from
//     Particle.getLightCoords; this engine keeps no per-cell light data on
//     the client, so a particle takes the one global light knob the terrain
//     takes (EntityEnvironment.hpp): the sky dim — night, night vision and
//     the Darkness pulse — or full block light for the types MC lights fully
//     (the huge explosion, the portal motes). They fog like the terrain. A
//     dust mote in a dark cave is as bright as the sky above it; that is
//     blocked on a client light engine, not on effort.
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
        // Resource pack reload: drop and reload the sprite textures.
        void ReloadTextures();

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
        // MC addAlwaysVisibleParticle's `alwaysShow` — the kinds only ever
        // spawned that way (the explosion emitter, the sulfur bubbles, the
        // noxious gas): the Minimal particle setting spares one in ten.
        static bool AlwaysShown(Game::ParticleKind kind);

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
        // Particle flag (AlwaysShown).
        bool ShouldSpawn(const Client::ClientLevelBridge::QueuedParticle& q,
                         const glm::vec3& cameraPos);

    private:
        // Sprite textures, one file each — index into m_textures.
        //   [0] heart  [1] angry
        //   [2..9]   generic_0..7   (smoke / large smoke / poof)
        //   [10..25] explosion_0..15
        //   [26..33] spell_0..7     (entity effect / witch)
        //   [34]     glint            (pause / reset mob growth)
        //   [36]     flame            (FlameParticle — the spawner's cage fire)
        //   [37]     bubble_white     (SulfurBubbleParticle)
        //   [38..45] noxious_gas_01..08  (NoxiousGasParticle)
        //   [46..53] geyser_base_01..08  (GeyserBaseParticle, GEYSER_BASE)
        //   [54..61] geyser_poof_01..08  (GeyserBaseParticle, GEYSER_POOF)
        //   [62..69] geyser_plume_01..08 (GeyserPlumeParticle)
        //   [35]     the block atlas — BORROWED from AtlasBuilder for the
        //            block marker (MC Layer.OPAQUE_TERRAIN: the marker's
        //            sprite is the block's `particle` texture, which lives
        //            on the blocks atlas). Re-fetched every Render, never
        //            destroyed here.
        static constexpr int kTexHeart     = 0;
        static constexpr int kTexAngry     = 1;
        static constexpr int kTexGeneric0  = 2;   // 8 frames
        static constexpr int kTexExplosion0 = 10; // 16 frames
        static constexpr int kTexSpell0    = 26;  // 8 frames
        static constexpr int kTexGlint     = 34;
        static constexpr int kTexAtlas     = 35;
        static constexpr int kTexFlame     = 36;  // flame (FlameParticle)
        static constexpr int kTexBubbleWhite  = 37;  // bubble_white (SulfurBubbleParticle)
        static constexpr int kTexNoxiousGas0  = 38;  // 8 frames
        static constexpr int kTexGeyserBase0  = 46;  // 8 frames
        static constexpr int kTexGeyserPoof0  = 54;  // 8 frames
        static constexpr int kTexGeyserPlume0 = 62;  // 8 frames
        static constexpr int kTextureCount = 70;
        static constexpr bool IsOwnedTexture(int texIndex) { return texIndex != kTexAtlas; }

        // MC SingleQuadParticle.Layer.TRANSLUCENT — the spell sheet and the
        // noxious gas (NoxiousGasParticle.getLayer). Every other particle
        // this engine spawns is OPAQUE (heart, poof, smoke, explode, huge
        // explosion, falling dust, block marker, the sulfur bubble, the
        // geyser's base, poof and plume), and each of those two sheets is
        // used by translucent kinds only, so the layer split is a split of
        // the texture buckets.
        static constexpr bool IsTranslucentTexture(int texIndex) {
            return (texIndex >= kTexSpell0 && texIndex < kTexSpell0 + 8) ||
                   (texIndex >= kTexNoxiousGas0 && texIndex < kTexNoxiousGas0 + 8);
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
            // MC Particle.setSize — the collision box MoveParticle sweeps:
            // 0.2 × 0.2 for every type but the sulfur bubble (0.02).
            float bbWidth = 0.2f, bbHeight = 0.2f;
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

            // MC PortalParticle.xStart/yStart/zStart — where the mote was
            // spawned. Its tick is a closed curve from here (out along the
            // velocity, back home, rising a block), not an integration, so
            // the start point has to be kept. The portal motes read all three;
            // yStart is also the sulfur bubble's yStart and the geyser
            // plume's startY.
            double xStart = 0, yStart = 0, zStart = 0;

            // The 26.3 geyser family's own state (see SpawnFromRequest):
            //   yEnd       SulfurBubbleParticle.yEnd / GeyserPlumeParticle.maxY
            //   yPrev      SulfurBubbleParticle.yPrev
            //   sizeMin    SulfurBubbleParticle.sizeStart / GeyserPlume.minSize
            //   sizeMax    GeyserPlumeParticle.maxSize
            //   propulsion GeyserPlumeParticle.initialPropulsion
            //   sprayX/Z   GeyserPlumeParticle.horizontalSprayX/Z
            //   done       GeyserPlumeParticle.done
            //   fadeStart  NoxiousGasParticle.fadeOutStartingPoint
            //   waterBlocks  GeyserParticleOptions.waterBlocks (the GEYSER seed)
            double yEnd = 0, yPrev = 0;
            float  sizeMin = 0.0f, sizeMax = 0.0f;
            float  propulsion = 0.0f;
            float  sprayX = 0.0f, sprayZ = 0.0f;
            bool   done = false;
            float  fadeStart = 0.0f;
            int    waterBlocks = 0;
            // MC SpriteSet.get(RandomSource): the frame a type picks ONCE at
            // spawn and keeps (PortalParticle's Provider), as opposed to the
            // age walk of setSpriteFromAge. Index into the type's sheet.
            uint8_t spriteFrame = 0;

            // The sprite's rectangle within its texture (MC TextureAtlasSprite
            // u0/u1/v0/v1). Whole texture for the one-file sprites; the
            // block marker's atlas cell otherwise.
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
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
        void LoadSprites();

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
