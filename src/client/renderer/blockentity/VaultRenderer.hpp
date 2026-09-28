// File: src/client/renderer/blockentity/VaultRenderer.hpp
//
// The item turning inside a vault's cage — MC VaultRenderer: while the vault
// shows something (VaultBlockEntity.Client.shouldDisplayActiveEffects), its
// shared display item is drawn at (0.5, 0.4, 0.5) in the block, spun by the
// client's VaultClientData (10 degrees a tick, rotLerp'd across the frame),
// through ItemEntityRenderer.renderMultipleFromCount at the vault cell's
// light (LevelRenderer.getLightCoords). The cage and the vault body are the
// block's ordinary model.
#pragma once

#include "BlockEntityRenderer.hpp"

namespace Render {

    class ItemEntityRenderer;

    class VaultRenderer : public BlockEntityRenderer {
    public:
        // The frame's item renderer. Owned by PlatformMain's join scope,
        // which outlives the block-entity pass; cleared before it shuts down.
        static void SetItemRenderer(ItemEntityRenderer* renderer);

        void Render(const Game::BlockEntity& be,
                    float partialTick,
                    const glm::mat4& proj,
                    const glm::mat4& view,
                    const glm::vec3& cameraPos) override;
    };

} // namespace Render
