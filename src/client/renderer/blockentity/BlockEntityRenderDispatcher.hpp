// File: src/client/renderer/blockentity/BlockEntityRenderDispatcher.hpp
//
// Owns one renderer per BlockEntityType. Per-frame, walks the block entities
// of the sections in the chunk renderer's draw list, distance-culls each,
// and calls the matching renderer. Single global instance
// (`g_blockEntityRenderDispatcher`) init'd in PlatformMain after the render
// backend is up.
//
// Mirrors MC `BlockEntityRenderDispatcher.java`; the enumeration is MC
// LevelRenderer.extractVisibleBlockEntities, which iterates
// `this.visibleSections` and reads each section mesh's renderable block
// entities — never the whole level. This used to snapshot and walk EVERY
// loaded chunk's BE map each frame with a horizontal distance sphere as the
// only cull; at a 32-chunk view that is ~4,000 map walks a frame for a
// handful of chests.
#pragma once

#include "BlockEntityRenderer.hpp"
#include "common/world/math/WorldMath.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace Game { class BlockEntity; }
namespace Client { class ClientChunkManager; struct ClientChunk; }

namespace Render {

    class BlockEntityRenderDispatcher {
    public:
        BlockEntityRenderDispatcher();
        ~BlockEntityRenderDispatcher();

        // Register a renderer for the given BlockEntityType id. Takes
        // ownership. Idempotent — replacing a renderer for the same id is
        // a no-op + warning (helps catch double-init bugs).
        void Register(uint16_t typeId, std::unique_ptr<BlockEntityRenderer> renderer);

        // Per-frame render pass over the block entities of this frame's
        // visible sections. Called from PlatformMain right after the chunk
        // solid pass and the block-break overlay, before portals.
        void RenderAll(Client::ClientChunkManager* chunkMgr,
                       const glm::mat4& proj,
                       const glm::mat4& view,
                       const glm::vec3& cameraPos,
                       float partialTick);

        // Bounded by BlockEntityTypeIds::MAX_ID. Direct array lookup; null
        // means "no renderer for this BE type" — placement still creates
        // the BE for state purposes, just nothing visual.
        BlockEntityRenderer* GetRenderer(uint16_t typeId) const;

        // The shared enumeration: the loaded chunks that own at least one
        // section in the chunk renderer's CURRENT draw list
        // (ChunkRenderer::GetMainViewSections), each once, in draw-list
        // order (near to far). A per-section visibility question is then
        // ChunkRenderer::IsSectionVisible — the caller walks a chunk's BE
        // map once and gates each entry by its section, rather than
        // touching the map once per visible section.
        //
        // Falls back to every loaded chunk when no chunk renderer is up (a
        // headless or half-initialised client), so nothing goes missing.
        //
        // `out` and `seen` are the caller's scratch, cleared and refilled
        // here; keeping them as members is what makes a steady-state frame
        // allocation-free. EndPortalRenderer calls this too, with its own
        // scratch, so both passes walk the same set. MAIN THREAD ONLY.
        using ChunkSet = std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>;
        static void CollectVisibleChunks(Client::ClientChunkManager* chunkMgr,
                                         std::vector<Client::ClientChunk*>& out,
                                         ChunkSet& seen);

    private:
        // Match BlockEntityTypeIds::MAX_ID = 64. Storing as a fixed array
        // keeps lookups branchless after the bounds check.
        std::array<std::unique_ptr<BlockEntityRenderer>, 64> m_renderers;

        // Per-frame scratch for CollectVisibleChunks.
        std::vector<Client::ClientChunk*> m_visibleChunks;
        ChunkSet m_seen;
    };

    extern std::unique_ptr<BlockEntityRenderDispatcher> g_blockEntityRenderDispatcher;

} // namespace Render
