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
// * WORLD LIGHTING: mobs render FULLBRIGHT. The entity shader has no light
//   input, and the client's entity-side block access reports a constant
//   brightness of 15 (ClientLevelBridge — real light data lives in the
//   chunk-mesh pipeline, not in IBlockAccess). Wiring a per-mob brightness
//   would need light-engine plumbing, so it is documented instead of
//   half-done. Note MC itself draws the magma cube, blaze and wither
//   fullbright (their getBlockLightLevel overrides return 15), so those
//   three are exact today.
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

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Frustum;
namespace Game { class Mob; }
namespace Client { class ClientMobManager; }

namespace Render {

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
        };

        ModelEntry* GetModelFor(Game::EntityTypeId type);
        TextureHandle LoadTexture(const std::string& relativePath);

        // The entity's MC transform chain, mapping model-space PIXELS to
        // camera-relative world space. Shared by the body and by anything
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
                                      float swimPivotY = 0.0f);

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
        std::unordered_map<std::string, TextureHandle> m_textureCache;

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
