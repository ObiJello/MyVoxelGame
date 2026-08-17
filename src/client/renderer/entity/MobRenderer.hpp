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
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include "client/renderer/entity/model/EntityModels.hpp"
#include "common/entity/EntityType.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game { class Mob; }
namespace Client { class ClientMobManager; }

namespace Render {

    class MobRenderer {
    public:
        MobRenderer() = default;
        ~MobRenderer();

        bool Initialize();
        void Shutdown();

        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos,
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
        // getFlipDegrees() is 90 for everything this port renders (MC overrides
        // it only for spiders, which flip 180). Public because the player
        // renderer topples its stick figures with the same curve.
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
        };

        ModelEntry* GetModelFor(Game::EntityTypeId type);
        TextureHandle LoadTexture(const std::string& relativePath);

        // The entity's MC transform chain, mapping model-space PIXELS to
        // camera-relative world space. Shared by the body and by anything
        // parented to it (the bow in a skeleton's hand), so the two can never
        // be composed from different matrices.
        // `deathFlipDeg` is MC LivingEntityRenderer.setupRotations' death roll,
        // already in degrees (see DeathFlipDegrees) — 0 for a living entity.
        static glm::mat4 EntityMatrix(const glm::dvec3& renderPos,
                                      const glm::vec3& cameraPos,
                                      float bodyRot, float ageScale,
                                      float deathFlipDeg = 0.0f,
                                      const glm::vec3& modelScale = glm::vec3(1.0f));

        // Build one mob's posed geometry into `verts`/`idx`, already in world
        // space. Returns the matrix it used.
        glm::mat4 AppendMob(EntityModel& model, const EntityRenderState& state,
                            const glm::dvec3& renderPos, float bodyRot,
                            const glm::vec3& cameraPos,
                            std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx);

        // Decode + extrude assets/textures/item/bow.png once, into MC item
        // model space. Returns false if the sprite is missing, in which case
        // the skeleton simply renders empty-handed rather than not at all.
        bool EnsureBowGeometry();

        // Append the bow, posed in the model's right hand.
        void AppendHeldBow(const EntityModel& model, const glm::mat4& entityMatrix,
                           std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx);

        ShaderHandle m_shader = INVALID_SHADER;
        BufferHandle m_vertexBuffer = INVALID_BUFFER;
        BufferHandle m_indexBuffer = INVALID_BUFFER;
        MeshHandle   m_mesh = INVALID_MESH;

        // Streaming capacity. A mob is ~1.5k vertices posed; this holds a few
        // hundred of them, well past what the tracking range can deliver.
        static constexpr size_t kMaxVertices = 262144;
        static constexpr size_t kMaxIndices  = 393216;

        std::unordered_map<uint16_t, ModelEntry> m_models;
        std::unordered_map<std::string, TextureHandle> m_textureCache;

        // The skeleton's bow, extruded once and kept in ITEM MODEL space so
        // each skeleton only pays a matrix multiply per vertex.
        enum class AssetState : uint8_t { Unloaded, Ready, Failed };
        AssetState m_bowState = AssetState::Unloaded;
        std::vector<ModelVertex> m_bowVerts;
        std::vector<uint32_t>    m_bowIndices;
        TextureHandle            m_bowTexture = INVALID_TEXTURE;

        // Reused across frames so the per-frame rebuild does not allocate.
        std::vector<ModelVertex> m_verts;
        std::vector<uint32_t>    m_indices;

        bool m_initialized = false;
    };

} // namespace Render
