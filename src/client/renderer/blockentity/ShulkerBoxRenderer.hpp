// File: src/client/renderer/blockentity/ShulkerBoxRenderer.hpp
//
// In-world shulker box renderer. Port of MC `ShulkerBoxRenderer.java` +
// `ShulkerModel.createBoxLayer()`.
//
// Shulker boxes have NO block model — assets/models/block/shulker_box.json
// carries a `particle` texture and nothing else, exactly like the chest,
// because vanilla draws them entirely through this renderer. Without it the
// chunk mesher emits no geometry and a placed shulker box is invisible.
//
// Geometry is two cuboids from the 64x64 shulker entity texture:
//   lid   texOffs(0, 0)  box(-8,-16,-8, 16x12x16)  pose offset (0, 24, 0)
//   base  texOffs(0, 28) box(-8, -8,-8, 16x 8x16)  pose offset (0, 24, 0)
// One mesh each, because the lid animates independently of the base.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/Direction.hpp"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

namespace Render {

    class ShulkerBoxRenderer : public BlockEntityRenderer {
    public:
        ShulkerBoxRenderer();
        ~ShulkerBoxRenderer() override;

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
        TextureHandle LoadColourTexture(const std::string& colour);
        // "shulker" for the undyed box, "shulker_red" etc. for the 16 dyes —
        // the file stem under assets/textures/entity/shulker/.
        const char*   TextureStemForBlock(Game::BlockID id) const;

        // MC's prepareModel + the two part poses, as one matrix per part.
        // `progress` is the lid-open amount in [0,1].
        glm::mat4 PartMatrix(bool lid, Game::Direction facing, float progress) const;

        enum Part { kBase = 0, kLid = 1, kPartCount = 2 };

        ShaderHandle m_shader = INVALID_SHADER;
        MeshHandle   m_mesh[kPartCount] = {INVALID_MESH, INVALID_MESH};
        BufferHandle m_vb[kPartCount]   = {INVALID_BUFFER, INVALID_BUFFER};
        BufferHandle m_ib[kPartCount]   = {INVALID_BUFFER, INVALID_BUFFER};
        uint32_t     m_indexCount[kPartCount] = {0, 0};
        std::unordered_map<std::string, TextureHandle> m_textureCache;
        bool         m_geomBuilt = false;
    };

} // namespace Render
