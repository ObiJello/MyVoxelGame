// File: src/client/renderer/blockentity/CopperGolemStatueRenderer.hpp
//
// Draws copper golem statues. Mirrors MC
// net.minecraft.client.renderer.blockentity.CopperGolemStatueBlockRenderer:
// the statue blocks' models are empty (particle only); the golem is drawn
// here in the block's pose (CopperGolemModel's body layer and its running,
// sitting and star pose layers, root turned over — CopperGolemStatueModel
// .setupAnim: root y 0, zRot π), facing the way the block faces
// (translation (0.5, 0, 0.5), yaw -facing.getOpposite().toYRot()), on the
// oxidation stage's copper_golem texture.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "BEModelMesh.hpp"

namespace Render {

    class CopperGolemStatueRenderer : public BlockEntityRenderer {
    public:
        CopperGolemStatueRenderer() = default;
        ~CopperGolemStatueRenderer() override;

        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

    private:
        static constexpr int kPoses = 4;   // standing, sitting, running, star
        ShaderHandle  m_shader = INVALID_SHADER;
        BEModel::Mesh m_pose[kPoses];      // copies: 4 facings × 2 light sets
        BEModel::TextureCache m_textures;
    };

} // namespace Render
