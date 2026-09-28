// File: src/client/renderer/blockentity/BannerRenderer.hpp
//
// Draws placed banners. Mirrors MC
// net.minecraft.client.renderer.blockentity.BannerRenderer with BannerModel /
// BannerFlagModel: the banner blocks' models are empty (particle only), so
// the pole, bar and flag are this renderer's — standing (16 rotation
// segments, pole + bar at y -44) or on a wall (four facings, bar and flag
// pushed out 9.5 / 10.5 px), under MODEL_TRANSLATION (0.5, 0, 0.5), the
// yaw and MODEL_SCALE (2/3, -2/3, -2/3). The flag sways (BannerFlagModel.
// setupAnim: xRot = (-0.0125 + 0.01 cos(2π phase)) π, phase from the
// position and game time); then submitPatterns layers the base colour and
// up to 16 patterns, each its entity/banner sheet tinted with its dye's
// diffuse colour (the bannerPattern render type — translucent).
#pragma once

#include "BlockEntityRenderer.hpp"
#include "BEModelMesh.hpp"

namespace Render {

    class BannerRenderer : public BlockEntityRenderer {
    public:
        BannerRenderer() = default;
        ~BannerRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

    private:
        // Lighting copies: orientation × light set. Ground banners have 16
        // orientations, wall banners 4 (Direction 2D data order).
        static constexpr int kGroundOrientations = 16;
        static constexpr int kWallOrientations = 4;

        ShaderHandle   m_shader = INVALID_SHADER;
        BEModel::Mesh  m_groundBody;   // pole + bar
        BEModel::Mesh  m_wallBody;     // bar
        BEModel::Mesh  m_groundFlag;
        BEModel::Mesh  m_wallFlag;
        BEModel::TextureCache m_textures;
        bool           m_built = false;
    };

} // namespace Render
