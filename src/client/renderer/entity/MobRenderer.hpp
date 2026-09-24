// File: src/client/renderer/entity/MobRenderer.hpp
//
// Draws every mob the client knows about.
//
// ── Why one draw per texture, not per mob or per part ─────────────────────
//
// ShulkerBoxRenderer draws one call per model PART, because it uses a static
// mesh and needs a fresh matrix each time. A mob is animated per entity per
// frame, so its geometry has to be rebuilt anyway — and once you are rebuilding
// it, applying the entity's world transform on the CPU at the same time costs
// nothing and collapses the whole scene into one draw per texture.
//
// That also sidesteps the Vulkan constraint that bit the block-entity
// renderers: only `uMVP` gets a matrix slot in the push-constant block, so a
// design that needed a separate model matrix per part would not fit.
//
// ── The MC transform chain ────────────────────────────────────────────────
//
// Model space is MC's: pixels, Y down, origin at the model root. Getting to
// world space is LivingEntityRenderer.submit's sequence, in order:
//
//   translate(entity position)
//   rotateY(180 - bodyRot)      MC's models face +Z at yaw 0
//   scale(-1, -1, 1)            flip to Y-up, and mirror X to match
//   scale(1/16)                 pixels -> blocks
//   translate(0, -1.501, 0)     EntityModel.MODEL_Y_OFFSET, in BLOCKS
//
// The 1.501 (not 1.5) is deliberate in MC: the extra thousandth lifts the model
// clear of the ground plane so a mob standing on a block does not z-fight it.
//
// ── Known visual deviations (deliberate, not oversights) ──────────────────
//
// * BLOB SHADOWS (MC EntityRenderDispatcher.renderBlockShadow) are not
//   rendered. MC projects a soft dark quad onto the top faces of the blocks
//   under the entity, per renderer shadowRadius (0.5 humanoid default,
//   slime size*0.25, ghast 1.5, ...), alpha fading with height. That needs
//   per-block ground shape queries from the render loop; skipped rather
//   than faked with a single floating quad.
//
// * WORLD LIGHTING: no per-cell light. The client keeps no light data for
//   entities (ClientLevelBridge reports a constant 15), so a mob takes the
//   one global knob the terrain takes (EntityEnvironment.hpp): the sky dim —
//   night, night vision, the Darkness pulse — per batch, full block light
//   for the entities MC lights at 15 (burning ones, blaze, magma cube,
//   wither, allay, vex, the fireballs), none for the EMISSIVE layers (eyes,
//   the warden's glow). Mobs fog exactly as the terrain does. A mob in a
//   torch-lit cave at night is as dark as the night; that waits on a client
//   light engine.
//
// * The warden's TENDRIL emissive layer is skipped: its alpha is
//   tendrilAnimation, which MC drives from vibration game events this port
//   does not run — the alpha is permanently 0, so MC would draw nothing
//   either. The other four warden emissive layers are rendered.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include "client/renderer/entity/EntityFrame.hpp"
#include "client/renderer/entity/model/EntityModels.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/Morph.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Frustum;
namespace Game {
    class ArmorStand; class Mob; }
namespace Client { class ClientMobManager; }

namespace Render {

    // The per-world "Baby Models" look (Options → World Settings).
    //
    // MC 26.1 ("Tiny Takeover") gave nine mobs dedicated baby meshes and
    // textures — cat, chicken, cow (+ mooshroom), ocelot, pig, rabbit, sheep,
    // wolf — and remodeled the adult rabbit with them. `New` draws those
    // (the generator's `<slug>_baby_new` / `rabbit_new` rows on the
    // `*_baby` sheets); `Classic` draws the pre-26.1 BabyModelTransform
    // babies. VISUAL ONLY: hitboxes, eye heights and the rabbit's 15-tick
    // hop clock follow 26.1 either way (see GeneratedEntityTypes' baby
    // columns and Rabbit::SetupAnimationStates).
    enum class BabyModelLook : uint8_t { New = 0, Classic = 1 };
    void SetBabyModelLook(BabyModelLook look);
    BabyModelLook GetBabyModelLook();
    // Bumped by SetBabyModelLook; the renderer drops its model cache when it
    // sees a new value, so a change applies to the next frame.
    int  BabyModelLookGeneration();

    class MobRenderer {
    public:
        MobRenderer() = default;
        ~MobRenderer();

        bool Initialize();
        void Shutdown();

        // `frustum` is the one the chunk pass of THIS view was culled with
        // (the main frustum, or a portal recursion's) — MC
        // extractVisibleEntities' shouldRender AABB test and the visible-
        // section gate both key on it; see EntityCulling.hpp.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, const Frustum& frustum,
                    const Client::ClientMobManager& mobs,
                    float partialTick);

        // MC's cull distance for mobs — clientTrackingRange in blocks, squared.
        // Beyond this the server has stopped sending updates anyway.
        static constexpr float kMaxRenderDistance = 160.0f;

        // ── /morph ────────────────────────────────────────────────────────
        // A player drawn as a mob: the mob's model at the player's feet with
        // the player's own body yaw, head yaw, pitch and walk animation, so
        // it reads exactly like a mob to whoever is looking. What a mob's
        // own state drives beyond that (held items, wool colour, anger,
        // charge, variants) has no player-side source and is not drawn —
        // the plain adult model with its base texture, its layer where the
        // model has one (sheep wool, drowned/stray/bogged overlay).
        struct MorphPose {
            uint32_t   code = Game::Morph::kNone;   // Game::Morph: a mob or a block here
            glm::dvec3 position;      // feet, world space (double)
            float bodyYaw   = 0.0f;   // degrees
            float headYaw   = 0.0f;
            float pitch     = 0.0f;
            float walkPos   = 0.0f;   // WalkAnimationState::PositionAt / SpeedAt
            float walkSpeed = 0.0f;
            float ageTicks  = 0.0f;   // the models' idle clock
            float scale     = 1.0f;   // the player's /scale
            int   hurtTime  = 0;      // red flash
            int   deathTime = 0;      // topple
            float swell     = 0.0f;   // creeper: 0..1 (Creeper.getSwelling)
            float animTick  = 0.0f;   // sheep graze / skeleton draw ticks left (lerped)
            uint32_t seed   = 0;      // per-body phase (phantom flap, witch nose)
            bool  crouching = false;  // the player model's sneak pose
            // The player's INVISIBILITY / GLOWING (MC: the body is not drawn
            // but its layers are; a glowing body is outlined) and burning
            // (full block light, MC getBlockLightLevel).
            bool  invisible = false;
            bool  glowing   = false;
            bool  onFire    = false;
            // A pitch of the whole body about its feet, after the body yaw —
            // the monster spawner's cage tilt (SpawnerRenderer: MC's
            // rotateX(-30) before the entity's own rotateY(180) is this +30
            // after it). 0 for a morph.
            float tiltDeg   = 0.0f;
        };
        // Same buffers, pipeline and per-batch draw as Render; appends
        // after it within the frame.
        void RenderMorphs(const glm::mat4& projection, const glm::mat4& view,
                          const glm::vec3& cameraPos, const Frustum& frustum,
                          const std::vector<MorphPose>& poses, float partialTick);

        // MC LivingEntityRenderer.setupRotations:174-181 — how far a dying
        // entity has toppled, in degrees, from its death timer.
        //
        //   fall = sqrt(min((deathTime - 1) / 20 * 1.6, 1)) * 90
        //
        // The sqrt front-loads it: the body is most of the way over within the
        // first few ticks and eases into flat, rather than rotating linearly.
        // getFlipDegrees() defaults to 90; MC overrides it to 180 for spiders,
        // silverfish and endermites, and the caller passes that. Public
        // because the player renderer topples its stick figures with the same
        // curve.
        static float DeathFlipDegrees(int deathTime, float partialTick,
                                      float flipDegrees = 90.0f) {
            if (deathTime <= 0) return 0.0f;
            float fall = (static_cast<float>(deathTime) + partialTick - 1.0f)
                       / 20.0f * 1.6f;
            fall = std::sqrt(std::max(fall, 0.0f));
            if (fall > 1.0f) fall = 1.0f;
            return fall * flipDegrees;
        }

    private:
        struct ModelEntry {
            std::unique_ptr<EntityModel> model;
            TextureHandle texture = INVALID_TEXTURE;
            // Sheep carry a second, dyed layer over the base body.
            std::unique_ptr<EntityModel> overlayModel;
            // MC AgeableMobRenderer's babyModel — the separate baby MESH
            // (big head, half body), not a shrunken adult. Built on the
            // first baby seen; null means MC has no baby mesh for this mob
            // and the uniform-shrink fallback applies.
            std::unique_ptr<EntityModel> babyModel;
            std::unique_ptr<EntityModel> babyOverlayModel;
            bool babyTried = false;
            // MC PufferfishRenderer's three puff stages — `model` is the
            // small (deflated) mesh; these are PUFFERFISH_MEDIUM/BIG,
            // swapped in per frame by the synced puff state.
            std::unique_ptr<EntityModel> pufferMid;
            std::unique_ptr<EntityModel> pufferBig;
            // MC CowRenderer / PigRenderer / ChickenRenderer's per-ModelType
            // AdultAndBabyModelPair: [variant byte] → mesh, adult and baby,
            // built on first sight. Null where the variant draws with the
            // NORMAL mesh (warm pig, warm chicken).
            std::unique_ptr<EntityModel> variantModels[3];
            std::unique_ptr<EntityModel> variantBabyModels[3];
            bool variantTried[3] = {false, false, false};
            // MC BreezeWindLayer / BreezeEyesLayer — the breeze mesh on its
            // own sheets (the wind one is 128x128, so it is a separate
            // generated row, not a redraw of the body's vertices).
            std::unique_ptr<EntityModel> windModel;
            std::unique_ptr<EntityModel> eyesModel;
            bool layersTried = false;
            // MC SulfurCubeInnerLayer — the inner cube (SULFUR_CUBE_INNER /
            // SULFUR_CUBE_SMALL_INNER) drawn under the translucent shell
            // while the cube holds no block.
            std::unique_ptr<EntityModel> innerModel;
            std::unique_ptr<EntityModel> innerBabyModel;
            bool innerTried = false;
        };

        ModelEntry* GetModelFor(Game::EntityTypeId type);
        // The baby mesh (and its overlay mesh) for a type, built on the
        // first baby seen — MC AgeableMobRenderer's babyModel. Shared by the
        // mob pass and the morph pass; entry.babyModel stays null for a mob
        // MC never draws through a baby mesh (the caller then shrinks the
        // adult).
        static void EnsureBabyModels(ModelEntry& entry, Game::EntityTypeId type);
        // `repeatWrap` samples the sheet with REPEAT instead of the clamp
        // entity sheets normally get — for a texture the shader scrolls
        // across its edge (the breeze's wind). Cached separately.
        TextureHandle LoadTexture(const std::string& relativePath, bool repeatWrap = false);

        // The entity's MC transform chain, mapping model-space PIXELS to
        // RENDER space (world minus the view's integer origin, see
        // RenderOrigin.hpp; `cameraPos` is no longer part of the
        // translation). Shared by the body and by anything
        // parented to it (the bow in a skeleton's hand), so the two can never
        // be composed from different matrices.
        // `scale` is MC LivingEntityRenderState.scale — the SCALE attribute,
        // NOT the baby shrink: MC babies render through a separate baby mesh
        // and this stays 1.0 for them. It carries kBabyScale only for the
        // fallback baby whose mesh could not be built.
        // `deathFlipDeg` is MC LivingEntityRenderer.setupRotations' death roll,
        // already in degrees (see DeathFlipDegrees) — 0 for a living entity.
        // `swimPitchDeg`/`swimPivotY` are DrownedRenderer.setupRotations' swim
        // tilt — a pitch about the mid-box pivot, applied after the death
        // flip, 0 for everything that is not a swimming drowned.
        static glm::mat4 EntityMatrix(const glm::dvec3& renderPos,
                                      const glm::vec3& cameraPos,
                                      float bodyRot, float scale,
                                      float deathFlipDeg = 0.0f,
                                      const glm::vec3& modelScale = glm::vec3(1.0f),
                                      float swimPitchDeg = 0.0f,
                                      float swimPivotY = 0.0f,
                                      const glm::vec3& modelOffset = glm::vec3(0.0f));

        // Build one mob's posed geometry into `verts`/`idx`, already in world
        // space. Returns the matrix it used.
        glm::mat4 AppendMob(EntityModel& model, const EntityRenderState& state,
                            const glm::dvec3& renderPos, float bodyRot,
                            const glm::vec3& cameraPos,
                            std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx);

        // One item model's display.thirdperson_righthand block — translation
        // in sixteenths of a block, rotation in degrees, uniform scale —
        // exactly as the item JSONs carry them.
        struct DisplaySpec {
            glm::vec3 translation;
            glm::vec3 rotationDeg;
            float     scale;
        };

        // MC ItemInHandLayer for an arbitrary flat item sprite: hand matrix
        // from the model, the layer's fixed grip, then the item's display
        // transform. Returns the sprite's texture (INVALID if unavailable).
        // The same, from a hand matrix the caller resolved (the armor
        // stand's off hand); `leftHand` mirrors the grip and the display
        // transform as MC ItemTransform.apply(leftHand) does.
        TextureHandle AppendHeldSpriteAt(const glm::mat4& entityMatrix,
                                         const glm::mat4& handMatrix, bool leftHand,
                                         const std::string& itemName,
                                         const DisplaySpec& spec,
                                         std::vector<ModelVertex>& verts,
                                         std::vector<uint32_t>& idx);
        // A BLOCK item in a hand: the block's own model through models/
        // block/block.json's thirdperson display (MC ItemInHandLayer with a
        // block model). Returns the blocks atlas.
        TextureHandle AppendHeldBlockAt(const glm::mat4& entityMatrix,
                                        const glm::mat4& handMatrix, bool leftHand,
                                        Game::BlockID block,
                                        std::vector<ModelVertex>& verts,
                                        std::vector<uint32_t>& idx);
        // MC CustomHeadLayer's non-skull branch: the item on the head — a
        // block's model, or a flat item through its HEAD display.
        TextureHandle AppendHeadItem(const glm::mat4& entityMatrix,
                                     const glm::mat4& headMatrix,
                                     const Game::ItemStack& stack,
                                     std::vector<ModelVertex>& verts,
                                     std::vector<uint32_t>& idx);
        // MC ArmorStandRenderer's layers: HumanoidArmorLayer (the four
        // armor pieces on the armor mesh, the leggings on the inner one),
        // ItemInHandLayer (both hands), CustomHeadLayer (a block or item on
        // the head). `emit(texture, firstIndex, cullBackFaces)` closes one
        // batch over everything appended since firstIndex.
        void AppendArmorStandLayers(const Game::ArmorStand& stand, const ArmorStandModel& model,
                                    const EntityRenderState& state, const glm::mat4& entityMatrix,
                                    const glm::dvec3& renderPos, float bodyRot,
                                    const glm::vec3& cameraPos,
                                    const std::function<void(TextureHandle, size_t, bool)>& emit);
        // The armor meshes for the stand, one per deformation (outer 1.0,
        // leggings 0.5) — MC ArmorModelSet.
        std::unique_ptr<ArmorStandArmorModel> m_armorStandArmorOuter;
        std::unique_ptr<ArmorStandArmorModel> m_armorStandArmorInner;
        TextureHandle AppendHeldSprite(const EntityModel& model,
                                       const glm::mat4& entityMatrix,
                                       const std::string& itemName,
                                       const DisplaySpec& spec,
                                       std::vector<ModelVertex>& verts,
                                       std::vector<uint32_t>& idx);

        // The drowned's trident: MC renders the trident MODEL in hand (the
        // trident_in_hand.json display block), not a sprite.
        TextureHandle AppendHeldTrident(const EntityModel& model,
                                        const glm::mat4& entityMatrix,
                                        std::vector<ModelVertex>& verts,
                                        std::vector<uint32_t>& idx);

        ShaderHandle m_shader = INVALID_SHADER;

        // Two streaming sets alternated per FRAME, each call within a frame
        // appending at a cursor — the scheme EntityCulling.hpp's frame-serial
        // note lays out. One set rewritten every call was a Vulkan hazard
        // twice over: the previous frame's commands could still be reading
        // it, and the portal pass's second call overwrote the main pass's
        // geometry before either had drawn.
        struct FrameBuffers {
            BufferHandle vb   = INVALID_BUFFER;
            BufferHandle ib   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
        };
        FrameBuffers m_frames[2];
        EntityFrame::Cursor m_frameCursor;
        size_t m_vertCursor = 0;   // vertices already written this frame
        size_t m_idxCursor  = 0;   // indices already written this frame

        // Streaming capacity PER SET, shared by every call in a frame. A mob
        // is ~1.5k vertices posed; this holds a few hundred of them, well
        // past what the tracking range can deliver.
        static constexpr size_t kMaxVertices = 262144;
        static constexpr size_t kMaxIndices  = 393216;

        std::unordered_map<uint16_t, ModelEntry> m_models;
        // Morph::Kind::Player: the wide player model (HumanoidModel, 64×64
        // skin), built on first use.
        std::unique_ptr<EntityModel> m_playerMorphModel;
        // A chicken morph's wing state, MC Chicken.aiStep's flap fields
        // stepped per tick from "airborne" (the morph's anim byte), one per
        // morphed player.
        struct MorphFlap {
            float flap = 0.0f, oFlap = 0.0f;
            float flapSpeed = 0.0f, oFlapSpeed = 0.0f;
            float flapping = 1.0f;
            int   lastTick = -1;
        };
        std::unordered_map<uint32_t, MorphFlap> m_morphFlaps;
        // An armadillo morph's shell: MC Armadillo.ArmadilloState run on the
        // morph's flag (Left Alt) — IDLE → ROLLING (10 ticks) → SCARED, and
        // UNROLLING (30) → IDLE — with the clips the mob's own
        // setupAnimationStates starts in each state, one per morphed player.
        struct MorphShell {
            int state = 0;          // Game::Armadillo::State
            int enteredTick = -1;
        };
        std::unordered_map<uint32_t, MorphShell> m_morphShells;
        std::unordered_map<std::string, TextureHandle> m_textureCache;
        int m_textureCacheGeneration = -1;   // Resources::CacheStale
        int m_babyLookGeneration = -1;       // BabyModelLookGeneration

        enum class AssetState : uint8_t { Unloaded, Ready, Failed };
        // A TridentModel instance reused for every in-hand trident (the
        // geometry under its "orient" wrapper is the raw vertical trident).
        std::unique_ptr<EntityModel> m_heldTridentModel;

        // MC ThrownItemRenderer — snowball, egg, splash potion and the
        // fireballs render as a camera-facing extruded item sprite at MC's
        // GROUND display scale (0.5). One cached mesh per item sprite.
        struct SpriteEntry {
            AssetState state = AssetState::Unloaded;
            std::vector<ModelVertex> verts;
            std::vector<uint32_t>    indices;
            TextureHandle            texture = INVALID_TEXTURE;
        };
        std::unordered_map<std::string, SpriteEntry> m_spriteEntries;

        SpriteEntry* EnsureSpriteGeometry(const std::string& itemName);
        // Appends the billboarded sprite; returns the texture to batch with,
        // or INVALID_TEXTURE when nothing was appended.
        TextureHandle AppendSpriteProjectile(const std::string& itemName,
                                             const glm::dvec3& renderPos,
                                             float halfHeight,
                                             const glm::vec3& cameraPos,
                                             std::vector<ModelVertex>& verts,
                                             std::vector<uint32_t>& idx);

        // ── End dragon fight geometry ─────────────────────────────────────
        //
        // MC EnderDragonRenderer.submitCrystalBeams — the 8-segment textured
        // tube between a crystal and its target (also the dragon-healing
        // beam), in world space, batched on end_crystal_beam.png (wrapped
        // REPEAT for the scroll). AppendDragonRays is the death cinematic's
        // fan of magenta rays, batched blended on a 1x1 white texture.
        TextureHandle BeamTexture();
        void AppendCrystalBeam(const glm::dvec3& baseWorld,
                               const glm::dvec3& tipWorld, float ageInTicks,
                               const glm::vec3& cameraPos,
                               std::vector<ModelVertex>& verts,
                               std::vector<uint32_t>& idx);
        void AppendDragonRays(const glm::dvec3& centerWorld, float deathTime01,
                              const glm::vec3& cameraPos,
                              std::vector<ModelVertex>& verts,
                              std::vector<uint32_t>& idx);
        TextureHandle m_whiteTexture = INVALID_TEXTURE;
        TextureHandle m_beamTexture = INVALID_TEXTURE;
        bool m_beamTextureTried = false;

        // Reused across frames so the per-frame rebuild does not allocate.
        std::vector<ModelVertex> m_verts;
        std::vector<uint32_t>    m_indices;

        bool m_initialized = false;
    };

} // namespace Render
