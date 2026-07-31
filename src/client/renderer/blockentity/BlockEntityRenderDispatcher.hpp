// File: src/client/renderer/blockentity/BlockEntityRenderDispatcher.hpp
//
// Owns one renderer per BlockEntityType. Per-frame, walks every loaded
// CLIENT chunk's BE map, frustum-culls + distance-culls each BE, and calls
// the matching renderer. Single global instance (`g_blockEntityRenderDispatcher`)
// init'd in PlatformMain after the render backend is up.
//
// Mirrors MC `BlockEntityRenderDispatcher.java`.
#pragma once

#include "BlockEntityRenderer.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace Game { class BlockEntity; }
namespace Client { class ClientChunkManager; }

namespace Render {

    class BlockEntityRenderDispatcher {
    public:
        BlockEntityRenderDispatcher();
        ~BlockEntityRenderDispatcher();

        // Register a renderer for the given BlockEntityType id. Takes
        // ownership. Idempotent — replacing a renderer for the same id is
        // a no-op + warning (helps catch double-init bugs).
        void Register(uint16_t typeId, std::unique_ptr<BlockEntityRenderer> renderer);

        // Per-frame render pass. Walks every loaded client chunk and renders
        // every BE within view distance. Called from PlatformMain right after
        // the chunk solid pass and the block-break overlay, before portals.
        void RenderAll(Client::ClientChunkManager* chunkMgr,
                       const glm::mat4& proj,
                       const glm::mat4& view,
                       const glm::vec3& cameraPos,
                       float partialTick);

        // Bounded by BlockEntityTypeIds::MAX_ID. Direct array lookup; null
        // means "no renderer for this BE type" — placement still creates
        // the BE for state purposes, just nothing visual.
        BlockEntityRenderer* GetRenderer(uint16_t typeId) const;

    private:
        // Match BlockEntityTypeIds::MAX_ID = 64. Storing as a fixed array
        // keeps lookups branchless after the bounds check.
        std::array<std::unique_ptr<BlockEntityRenderer>, 64> m_renderers;
    };

    extern std::unique_ptr<BlockEntityRenderDispatcher> g_blockEntityRenderDispatcher;

} // namespace Render
