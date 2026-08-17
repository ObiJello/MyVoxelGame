// File: src/client/renderer/blockentity/BlockEntityRenderers.cpp
#include "BlockEntityRenderers.hpp"
#include "BlockEntityRenderDispatcher.hpp"
#include "ChestRenderer.hpp"
#include "CampfireRenderer.hpp"
#include "ShulkerBoxRenderer.hpp"
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

        // Shulker box — all 17 colours share one BE type, and the renderer
        // picks the entity texture per blockId inside Render, the same way the
        // chest picks normal/trapped/ender. Like the chest, its block model is
        // element-less, so without this the placed block is invisible.
        auto shulker = std::make_unique<ShulkerBoxRenderer>();
        if (shulker->Initialize()) {
            g_blockEntityRenderDispatcher->Register(
                Game::BlockEntityTypeIds::SHULKER_BOX, std::move(shulker));
        } else {
            Log::Error("[BERenderers] ShulkerBoxRenderer init failed");
        }

        // Campfire + soul campfire. Register() takes ownership, so each type
        // id needs its own instance; they are stateless past the shader, so
        // the duplication costs one shader handle.
        for (uint16_t typeId : {Game::BlockEntityTypeIds::CAMPFIRE,
                                Game::BlockEntityTypeIds::SOUL_CAMPFIRE}) {
            auto campfire = std::make_unique<CampfireRenderer>();
            if (campfire->Initialize()) {
                g_blockEntityRenderDispatcher->Register(typeId, std::move(campfire));
            } else {
                Log::Error("[BERenderers] CampfireRenderer init failed (type %u)",
                           static_cast<unsigned>(typeId));
            }
        }
    }

} // namespace Render
