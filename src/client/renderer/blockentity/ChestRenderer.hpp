// File: src/client/renderer/blockentity/ChestRenderer.hpp
//
// In-world chest renderer. Builds the MC-equivalent chest geometry (base +
// lid + lock) and draws it depth-tested into the world. Texture is the chest
// entity atlas (assets/textures/entity/chest/{normal,trapped,ender}.png) —
// loaded per-variant on first use.
//
// Lid open animation will arrive in Stage 4 when ContainerOpenersCounter
// + BlockEntityActionS2C are wired; until then the lid is drawn closed.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/world/block/Blocks.hpp"
#include <unordered_map>
#include <string>

namespace Render {

    class ChestRenderer : public BlockEntityRenderer {
    public:
        ChestRenderer();
        ~ChestRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

        void RenderBEWLR(Game::BlockID blockId, const glm::mat4& mvp) override;
        // Owns its item geometry — see BlockEntityRenderer::SupportsBEWLR.
        bool SupportsBEWLR() const override { return true; }

    private:
        TextureHandle LoadVariantTexture(const std::string& variant);
        const char*   VariantForBlock(Game::BlockID id) const;

        // MC ChestModel has THREE layer definitions — single, double-left and
        // double-right — and ChestRenderer picks one per chest from its
        // ChestType. Same here: one mesh per variant, chosen at draw time.
        enum Variant { kSingle = 0, kLeft = 1, kRight = 2, kVariantCount = 3 };

        ShaderHandle  m_shader = INVALID_SHADER;
        MeshHandle    m_mesh[kVariantCount] = {INVALID_MESH, INVALID_MESH, INVALID_MESH};
        BufferHandle  m_vb[kVariantCount]   = {INVALID_BUFFER, INVALID_BUFFER, INVALID_BUFFER};
        BufferHandle  m_ib[kVariantCount]   = {INVALID_BUFFER, INVALID_BUFFER, INVALID_BUFFER};
        std::unordered_map<std::string, TextureHandle> m_textureCache;

        bool          m_geomBuilt    = false;
        uint32_t      m_indexCount[kVariantCount] = {0, 0, 0};
    };

} // namespace Render
