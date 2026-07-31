// File: src/client/renderer/blockentity/BlockEntityRenderers.cpp
#include "BlockEntityRenderers.hpp"
#include "BlockEntityRenderDispatcher.hpp"
#include "ChestRenderer.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/core/Log.hpp"

namespace Render {

    void RegisterAllBlockEntityRenderers() {
        if (!g_blockEntityRenderDispatcher) {
            g_blockEntityRenderDispatcher = std::make_unique<BlockEntityRenderDispatcher>();
        }

        // Chest (also serves trapped + ender, same renderer with variant
        // texture switched per blockId inside Render). Stages 6/8/9/10 will
        // add more entries here as their renderers ship.
        auto chest = std::make_unique<ChestRenderer>();
        if (chest->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::CHEST, std::move(chest));
        } else {
            Log::Error("[BERenderers] ChestRenderer init failed");
        }
    }

} // namespace Render
