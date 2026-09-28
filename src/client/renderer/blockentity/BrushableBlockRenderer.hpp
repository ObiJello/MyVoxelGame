// File: src/client/renderer/blockentity/BrushableBlockRenderer.hpp
//
// Draws the find poking out of a suspicious sand / gravel block while it is
// being brushed. Mirrors MC
// net.minecraft.client.renderer.blockentity.BrushableBlockRenderer.
//
// The block itself is the ordinary chunk mesh (its model follows DUSTED);
// this renderer only adds the item, and only once there is something to
// show: DUSTED > 0, a hit direction and a rolled item (the server rolls the
// archaeology table on the first stroke and syncs the result). The item
// slides out of the brushed face 7.5% of a block per dusted stage, laid as
// MC lays it (75 + 11 degrees about Y, a quarter turn more on an east/west
// face, half size), lit by the cell in front of that face.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"

namespace Render {

    class BrushableBlockRenderer : public BlockEntityRenderer {
    public:
        bool Initialize();
        void Shutdown();

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;

    private:
        ShaderHandle m_shader = INVALID_SHADER;
    };

} // namespace Render
