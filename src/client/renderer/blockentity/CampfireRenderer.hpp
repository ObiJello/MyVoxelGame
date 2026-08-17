// File: src/client/renderer/blockentity/CampfireRenderer.hpp
//
// Draws the food lying on a campfire. Mirrors MC
// net.minecraft.client.renderer.blockentity.CampfireRenderer.
//
// The fire itself is NOT drawn here — that is the block model, chosen by the
// `lit` blockstate (campfire vs campfire_off), so it comes out of the ordinary
// chunk mesh. This renderer only adds the up-to-four items, which cannot live
// in a chunk mesh because they change without the block changing.
//
// Each item is a flat sprite laid face-up on the fire, rotated a quarter turn
// per slot so four items fan out around the middle rather than stacking. The
// per-slot yaw is MC's `Direction.from2DDataValue((slot + facing.get2DDataValue()) % 4)`
// — the block's own facing is the starting offset, so rotating a campfire
// carries its food around with it.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "../backend/RenderTypes.hpp"

namespace Render {

    class CampfireRenderer : public BlockEntityRenderer {
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
