// File: src/client/renderer/blockentity/BellRenderer.hpp
//
// Draws the bell's body. Mirrors MC
// net.minecraft.client.renderer.blockentity.BellRenderer / BellModel: the
// bell blocks' models are the frame only; the body (bell_body 6x7x6 hung at
// (8, 12, 8), bell_base 8x2x8 under it) is this renderer's, on
// entity/bell/bell_body, swinging away from the struck face while the
// BellBlockEntity shakes: baseRot = sin(ticks / π) / (4 + ticks / 3) about X
// (north / south) or Z (east / west).
#pragma once

#include "BlockEntityRenderer.hpp"
#include "BEModelMesh.hpp"

namespace Render {

    class BellRenderer : public BlockEntityRenderer {
    public:
        BellRenderer() = default;
        ~BellRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

    private:
        ShaderHandle  m_shader = INVALID_SHADER;
        BEModel::Mesh m_body;   // two copies: default and nether light
        BEModel::TextureCache m_textures;
    };

} // namespace Render
