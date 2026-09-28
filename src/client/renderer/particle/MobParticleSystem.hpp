// File: src/client/renderer/particle/MobParticleSystem.hpp
//
// The client particle engine — MC's ParticleEngine + ParticleResources with
// every provider 26.3 registers (client/particle/*.java), plus the engine's
// own kinds (the Hush / Aether / Aurelith motes). The name is historical:
// it began as the handful of particles the mob port needed.
//
// ── What is ported ────────────────────────────────────────────────────────
//   * Particle / SingleQuadParticle / NoRenderParticle and every concrete
//     type: constructors, tick(), move() overrides, getQuadSize, getLayer,
//     getLightCoords, getFacingCameraMode and the custom extract()s (the
//     shriek's two tilted quads, the vibration's aimed pair, the firework
//     spark's twinkle, the dust transition's colour lerp, the flash's alpha)
//     — constants verbatim, integrated at MC's fixed 20 Hz with sub-tick
//     render interpolation, including Particle.move's block collision.
//   * ParticleResources' sprite sets from particles/<type>.json on ONE
//     particle atlas (ParticleSprites.hpp), setSpriteFromAge / get(random).
//   * ParticleEngine's groups: SINGLE_QUADS and NO_RENDER each capped at
//     16384 with MC's 12288..16384 reservoir (accept with probability
//     free², ParticleGroup.add), ParticleLimit.SPORE_BLOSSOM (1000), and
//     ClientLevel.doAddParticle's limiter (32 blocks unless overrideLimiter;
//     the Particles option's calculateParticleLevel).
//   * SingleQuadParticle.Layer: OPAQUE / TRANSLUCENT on the particle atlas,
//     and TerrainParticle / BreakingItemParticle's Layer.bySprite onto the
//     block and item atlases (translucent for a translucent block).
//   * Lighting: Particle.getLightCoords at the particle's cell through the
//     client lightmap (EntityEnvironment::LightColor), with each type's
//     override (FULL_BRIGHT, block light 15, addSmoothBlockEmission ramps,
//     the firefly's own glow).
//
// ── Rendering ─────────────────────────────────────────────────────────────
// CPU-built quads in render space (camera-relative: RenderOrigin.hpp), one
// streaming VB per Render call, one draw per (sheet, layer): OPAQUE first —
// depth write on, no blending, the shader's 0.1 alpha cutout shapes it —
// then TRANSLUCENT, blended, depth write off. Each vertex carries its
// particle's colour × lightmap colour; the draw's own light is 1.
//
// REMAINING DEVIATIONS
//   * No frustum cull per particle (QuadParticleGroup's pointInFrustum); the
//     GPU clips instead.
//   * No OIT / sort within the translucent pass (MC's non-Fabulous path
//     does not sort either).
//   * ELDER_GUARDIAN draws its ghost as the elder guardian's texture on a
//     camera-locked quad sequence only when the entity model renderer is
//     unavailable — see the header note on RenderElderGuardians.
//
// Spawn requests arrive through ClientLevelBridge's queue (the client half of
// MC Level.addParticle) — drained here every Update. Main thread only.

#pragma once

#include "../backend/RenderTypes.hpp"
#include "ParticleSprites.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Game { struct IBlockAccess; }

namespace Render {

    class MobParticleSystem {
    public:
        MobParticleSystem();
        ~MobParticleSystem();

        bool Initialize();
        void Shutdown();

        // The OpenGL particle shader's sources (MC core/particle): texture ×
        // vertex colour, the 0.1 alpha cutout, the draw's light, the terrain
        // fog. The weather columns share it, as MC's WEATHER pipeline shares
        // the particle shader (WeatherEffectRenderer); Vulkan loads the same
        // shader as shaders/mob_particle_vk.*.spv.
        static const char* VertexSource()   { return s_vertSource; }
        static const char* FragmentSource() { return s_fragSource; }
        // Resource pack reload: rebuild the particle atlas and sprite sets.
        void ReloadTextures();

        // Drain queued spawn requests from every client level and advance
        // the simulation. dt is real frame time; internally the particles
        // step at MC's fixed 20 Hz, with the remainder kept as the render
        // partial tick.
        //
        // Every client level's queue is drained (the active one and the
        // far sides seen through immersive portals); each particle remembers
        // its level and collides against that level's blocks. The distance
        // cull for a far level is measured from the camera's image through
        // the nearest portal that leads there.
        void Update(float dt, const glm::vec3& cameraPos);

        // Draw every live particle of `dimension` — the main pass draws the
        // active level's, a portal view draws its far level's.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, Game::DimensionId dimension);

        size_t Count() const { return m_particles.size(); }
        // MC ParticleEngine.countParticles: "SQ <n> NR <n> EG <n> T <total>".
        std::string DebugCounts() const;

        // MC ClientLevel.doAddParticle's limiter: a particle further than 32
        // blocks from the camera is DROPPED unless its type (or the request)
        // overrides the limit.
        static constexpr double kParticleCutoffSq = 1024.0;   // 32 blocks
        // MC ParticleType.getOverrideLimiter() (ParticleTypes' table).
        static bool OverridesParticleLimiter(Game::ParticleKind kind);
        // The kinds the engine only ever spawns through
        // addAlwaysVisibleParticle (the explosion emitter, the sulfur
        // bubbles, the noxious gas) — a request queued without the flag
        // still gets it for them.
        static bool AlwaysShown(Game::ParticleKind kind);

        // MC ClientLevel.doAddParticle, all three gates in MC's order:
        //
        //     status = calculateParticleLevel(alwaysShow)
        //     if (overrideLimiter)            create
        //     else if (distSq > 1024)         drop
        //     else if (status == MINIMAL)     drop
        //     else                            create
        bool ShouldSpawn(const Client::ClientLevelBridge::QueuedParticle& q,
                         const glm::vec3& cameraPos);

    private:
        // MC ParticleRenderType — the engine's groups.
        enum class Group : uint8_t { SingleQuads, NoRender, ElderGuardians };
        // The texture a quad samples (SingleQuadParticle.Layer's atlas).
        enum class Sheet : uint8_t { Particles = 0, Blocks = 1, Items = 2, ElderGuardian = 3 };
        static constexpr int kSheetCount = 4;
        // MC SingleQuadParticle.FacingCameraMode, plus the two types that
        // build their own rotations in extract().
        enum class Facing : uint8_t { LookAtXYZ, LookAtY, Shriek, Vibration };
        // MC Particle.getLightCoords and its overrides.
        enum class Light : uint8_t {
            World,             // the cell's light (Particle.getLightCoords)
            FullBright,        // 15728880
            BlockFull,         // LightCoordsUtil.withBlock(world, 15)
            AgeEmission,       // addSmoothBlockEmission(world, (age + a) / lifetime)
            AgeEmissionPow4,   // addSmoothBlockEmission(world, (age / lifetime)^4)
            Firefly,           // (int)(255 * fade(progress, 0.1, 0.3))
        };
        // DripParticle's subclasses.
        enum class Drip : uint8_t { None, Hang, CoolingHang, Falling, FallAndLand, DripstoneFallAndLand,
                                    HoneyFallAndLand, Land };
        enum class DripFluid : uint8_t { Empty, Water, Lava };

        // One particle — MC Particle + SingleQuadParticle state and the
        // per-type fields of the subclasses, doubles where MC uses doubles.
        struct Particle {
            Game::ParticleKind kind = Game::ParticleKind::Smoke;
            Game::DimensionId  dimension = Game::DimensionId::Overworld;
            Group  group = Group::SingleQuads;
            double x = 0, y = 0, z = 0;
            double xo = 0, yo = 0, zo = 0;
            double xd = 0, yd = 0, zd = 0;
            int    age = 0;
            int    lifetime = 0;
            float  gravity = 0.0f;
            float  friction = 0.98f;
            bool   speedUpWhenYMotionIsBlocked = false;
            bool   hasPhysics = true;
            // MC Particle.move overridden to a plain box shift (FlameParticle,
            // EndRodParticle, PortalParticle, SuspendedTownParticle, …).
            bool   freeMove = false;
            // MC Particle.setSize — the collision box move() sweeps.
            float  bbWidth = 0.2f, bbHeight = 0.2f;
            bool   onGround = false;
            bool   stoppedByCollision = false;
            bool   removed = false;
            bool   limitedSporeBlossom = false;   // ParticleLimit.SPORE_BLOSSOM
            float  quadSize = 0.1f;
            float  rCol = 1.0f, gCol = 1.0f, bCol = 1.0f, alpha = 1.0f;
            float  roll = 0.0f, oRoll = 0.0f;
            float  rollSpeed = 0.0f;      // FallingDustParticle.rotSpeed

            // Sprite: `spriteSet` walked by age (setSpriteFromAge) when
            // `walk`, else the fixed `sprite`, both on the particle atlas.
            // Terrain / item particles sample the block or item atlas at
            // (u0, v0)-(u1, v1) instead.
            Sheet   sheet = Sheet::Particles;
            int16_t spriteSet = -1;
            bool    walk = false;
            int32_t sprite = -1;
            float   u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            // A particle-atlas sprite drawn through a sub-rectangle given as
            // fractions in u0..v1 (BreakingItemParticle's sulfur cube goo).
            bool    subRect = false;
            bool    translucent = false;
            Light   light = Light::World;
            Facing  facing = Facing::LookAtXYZ;

            // PortalParticle / FlyTowardsPositionParticle /
            // FlyStraightTowardsParticle's start; the sulfur bubble's and the
            // geyser plume's yStart.
            double xStart = 0, yStart = 0, zStart = 0;

            // The 26.3 geyser family (see SpawnFromRequest).
            double yEnd = 0, yPrev = 0;
            float  sizeMin = 0.0f, sizeMax = 0.0f;
            float  propulsion = 0.0f;
            float  sprayX = 0.0f, sprayZ = 0.0f;
            bool   done = false;
            float  fadeStart = 0.0f;
            int    waterBlocks = 0;
            uint8_t spriteFrame = 0;   // the engine motes' twinkle phase / mist frame

            // SimpleAnimatedParticle's fade colour; FireworkParticles'
            // SparkParticle's trail / twinkle.
            float  fadeR = 0.0f, fadeG = 0.0f, fadeB = 0.0f;
            bool   hasFade = false;
            bool   trail = false, twinkle = false;

            // DripParticle.
            Drip      drip = Drip::None;
            DripFluid dripFluid = DripFluid::Empty;
            Game::ParticleKind chainKind = Game::ParticleKind::Smoke;   // falling / landing particle

            // FallingParticle (the falling leaves).
            float  rotSpeed = 0.0f, spinAcceleration = 0.0f, windBig = 0.0f;
            bool   swirl = false, flowAway = false;
            double xaFlowScale = 0.0, zaFlowScale = 0.0, swirlPeriod = 0.0;

            // SpellParticle.originalAlpha.
            float  originalAlpha = 1.0f;
            // FlyTowardsPositionParticle.lifetimeAlpha.
            float  laStart = 1.0f, laEnd = 1.0f, laStartAt = 0.0f, laEndAt = 1.0f;
            // FlyStraightTowardsParticle's colours (ARGB).
            uint32_t startColor = 0, endColor = 0;
            // DustColorTransitionParticle.
            glm::vec3 fromColor{1.0f}, toColor{1.0f};
            // VibrationSignalParticle / TrailParticle target.
            glm::dvec3 target{0.0};
            int32_t targetEntity = -1;
            float  targetYOffset = 0.0f;
            float  rot = 0.0f, rotO = 0.0f, pitch = 0.0f, pitchO = 0.0f;
            // ShriekParticle.delay.
            int    delay = 0;
            // WaterCurrentDownParticle.angle.
            float  angle = 0.0f;
            // DragonBreathParticle.hasHitGround.
            bool   hasHitGround = false;
            // GustSeedParticle.
            double seedScale = 0.0;
            int    seedDelay = 0;
            // FireworkParticles.Starter.
            std::shared_ptr<const std::vector<Game::FireworkExplosion>> explosions;
            int    life = 0;
            bool   twinkleDelay = false;
            bool   playSound = false;
        };

        // A spawn a particle makes while it ticks — its level's queue, drained
        // after the sweep (the emitters, the drips' landing, the lava's smoke).
        struct ChildSpawn {
            Client::ClientLevelBridge::QueuedParticle q;
            // FireworkParticles.SparkParticle's trail spawns bypass the
            // provider (engine.add of a copied spark): carried whole.
            bool direct = false;
            Particle particle;
        };

        void SpawnFromRequest(const Client::ClientLevelBridge::QueuedParticle& q,
                              Game::DimensionId dimension);
        // The provider half (ParticleProviders.cpp): builds `p` for `q`;
        // false when MC's provider returns null (an air block particle, an
        // unknown item …).
        bool MakeParticle(Particle& p, const Client::ClientLevelBridge::QueuedParticle& q,
                          const Game::ParticleOptions& options, const Game::IBlockAccess* blocks);
        // MC ParticleEngine.add → ParticleGroup.add: the group caps, the
        // reservoir and the particle limits.
        void AddParticle(Particle&& p);
        // Where the 32-block spawn cull is measured from for a level: the
        // camera, or its image through the nearest portal into that level.
        static glm::vec3 AnchorFor(Game::DimensionId dimension, const glm::vec3& cameraPos);
        void TickParticle(Particle& p, const Game::IBlockAccess* blocks, std::vector<ChildSpawn>& spawns);
        // MC Particle.move — per-axis block collision for hasPhysics types.
        void MoveParticle(Particle& p, double xa, double ya, double za,
                          const Game::IBlockAccess* blocks);
        // MC Particle.setSize, keeping the box centred on x/z.
        static void SetSize(Particle& p, float w, float h);
        // MC Particle.scale(s) (SingleQuadParticle's override included).
        static void Scale(Particle& p, float s);
        // MC Particle.setPower.
        static void SetPower(Particle& p, float power);

        // Sprite, size, light and layer at render time.
        int  SpriteFor(const Particle& p) const;
        float QuadSizeFor(const Particle& p, float partialTick) const;
        int  LightCoordsFor(const Particle& p, float partialTick) const;
        bool TranslucentFor(const Particle& p) const;
        // The sprite set of a kind's type (-1 for none), cached per kind.
        int  SetOf(Game::ParticleKind kind) const;

        // Queue a child spawn in the same form a level's queue holds.
        static ChildSpawn Child(Game::ParticleKind kind, double x, double y, double z,
                                double xd, double yd, double zd);

        std::vector<Particle> m_particles;
        std::vector<Client::ClientLevelBridge::QueuedParticle> m_incoming;
        Game::JavaRandom m_rng;
        // MC ClientLevel.random, which calculateParticleLevel draws from —
        // a different stream from the ParticleEngine's, so the limiter's
        // rolls do not perturb the particle constructors' sequence.
        Game::JavaRandom m_limiterRng;
        // The Particles option, read once per Update on the main thread.
        Game::ParticleStatus m_particleStatus = Game::ParticleStatus::All;
        // Live counts per group and for the spore-blossom limit.
        size_t m_groupCount[3] = {0, 0, 0};
        int    m_sporeBlossomCount = 0;
        // Client ticks run (for the animated sprites' frame).
        int64_t m_ticks = 0;
        // The camera's world position of the last Update (the scoping check
        // SpellParticle makes, PlayerCloudParticle's nearest player).
        glm::dvec3 m_camera{0.0};

        float m_tickAccum = 0.0f;   // seconds toward the next 20 Hz step
        float m_partialTick = 0.0f; // 0..1 for render interpolation

        ShaderHandle  m_shader = INVALID_SHADER;
        ParticleSpriteAtlas m_sprites;
        mutable std::vector<int16_t> m_setByKind;   // SetOf cache
        TextureHandle m_elderGuardianTexture = INVALID_TEXTURE;

        // One streaming vertex buffer per Render CALL, not per frame. The
        // system draws once per view — the main one and every portal view —
        // and the Vulkan backend records an upload immediately but runs
        // the draws at submit: a second upload into the same buffer
        // clobbers what the first draw will read, and growing that buffer
        // destroys one a recorded draw still references — VK_ERROR_
        // DEVICE_LOST. Calls cycle through the ring; a slot grows on its own,
        // deferred.
        struct StreamSlot {
            BufferHandle vb = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            size_t       capacityVerts = 0;
        };
        // PER FRAME, not one shared ring (the fixed 16 was shared by
        // consecutive frames): Vulkan records the next frame while the last
        // one still executes, into persistently mapped buffers, so a frame
        // with more than ~8 calls — a leave-capture frame draws the system
        // once per panorama tile plus each tile's portal views, on top of
        // its own calls — wrapped onto the slot the previous frame was still
        // drawing from, and that frame showed another view's particles for a
        // frame. Each frame now takes fresh slots from its own set
        // (BeginFrame rotates; three sets > the two frames in flight),
        // growing it to the frame's call count. Same scheme as
        // PortalParticleSystem.
        static constexpr size_t kFrameSets        = 3;
        static constexpr size_t kMaxSlotsPerFrame = 64;   // wrap guard (BeginFrame never called)
        std::array<std::vector<StreamSlot>, kFrameSets> m_frameSlots;
        size_t m_frameSet   = 0;
        size_t m_slotCursor = 0;
        StreamSlot& AcquireSlot(size_t vertsNeeded, size_t minCapacity);
    public:
        // Once per frame, before any Render call (PlatformMain, beside the
        // backend's BeginFrame): the next set of stream slots.
        void BeginFrame() {
            m_frameSet   = (m_frameSet + 1) % kFrameSets;
            m_slotCursor = 0;
        }
    private:
        void DestroySlots();

        static const char* s_vertSource;
        static const char* s_fragSource;
    };

    extern MobParticleSystem g_mobParticleSystem;

} // namespace Render
