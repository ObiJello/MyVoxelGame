// File: src/client/renderer/entity/ItemEntityRenderer.hpp
//
// Draws dropped items lying in the world — MC's ItemEntityRenderer.
//
// Geometry comes from the same builders the first-person hand uses
// (Render::BuildBlockModelMesh for block items, HeldItemSpriteMesh for sprite
// items), so a cobblestone on the ground and a cobblestone in your hand are the
// same model. What differs is the transform: world space instead of view space,
// the item model's `display.ground` scale, and MC's bob + spin.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"   // ItemCubeVert
#include "common/entity/Item.hpp"
#include "common/entity/ItemEntity.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace Render {

    class ItemEntityRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Draw every entity in Client::g_itemEntityManager.
        //
        // `partialTick` (0..1) is how far through the current 50 ms client tick
        // this frame is, used to blend each entity's previous-tick position
        // with its current one. Without it items would jump once per tick
        // rather than moving smoothly — the same reason PlayerRenderer takes
        // it.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, float partialTick);

    private:
        // Draw one item at a world position with MC's bob + spin + ground
        // transform. Shared by the entities lying in the world and by the
        // pickup animations flying into a player, so the two can never drift
        // apart visually.
        //
        // `ageTicks` drives bob and spin; a pickup animation passes the frozen
        // age it was captured with, which is what keeps a collected item from
        // continuing to rotate as it flies.
        void DrawItem(const Game::ItemStack& stack, const glm::vec3& worldPos,
                      float ageTicks, float bobOffs,
                      const glm::mat4& viewProj, const glm::vec3& cameraPos,
                      std::vector<ItemCubeVert>& verts,
                      std::vector<uint32_t>& idx);

        bool m_initialized = false;

        ShaderHandle  m_shader       = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // Streaming scratch buffers for the block-item path, reused across all
        // entities in a frame — one item's mesh is uploaded, drawn, and
        // overwritten by the next.
        BufferHandle m_cubeVB   = INVALID_BUFFER;
        BufferHandle m_cubeIB   = INVALID_BUFFER;
        MeshHandle   m_cubeMesh = INVALID_MESH;

        // MC Entity.shouldRenderAtSqrDistance: an entity renders while
        // `distance² < (boundingBox.getSize() * 64 * viewScale)²`, where
        // getSize() is the mean of the box's three extents. A 0.25³ item gives
        // 0.25 * 64 = 16 blocks — small entities cull aggressively in vanilla,
        // and dropped items really do wink out at around that range.
        //
        // viewScale is MC's "Entity Distance" video option, 1.0 at the default
        // 100%. Raise this multiplier if items should stay visible further out;
        // it is the one knob here that is a deliberate look choice.
        static constexpr float kViewScale = 1.0f;

        static constexpr float kMaxRenderDistance =
            ((Game::ItemEntity::kWidth + Game::ItemEntity::kHeight
              + Game::ItemEntity::kWidth) / 3.0f) * 64.0f * kViewScale;
    };

} // namespace Render
