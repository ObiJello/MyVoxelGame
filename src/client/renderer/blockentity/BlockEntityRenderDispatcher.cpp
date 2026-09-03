// File: src/client/renderer/blockentity/BlockEntityRenderDispatcher.cpp
#include "BlockEntityRenderDispatcher.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "common/core/Config.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/chunk/Chunk.hpp"
#include <vector>

namespace Render {

    std::unique_ptr<BlockEntityRenderDispatcher> g_blockEntityRenderDispatcher;

    BlockEntityRenderDispatcher::BlockEntityRenderDispatcher() = default;
    BlockEntityRenderDispatcher::~BlockEntityRenderDispatcher() = default;

    void BlockEntityRenderDispatcher::Register(uint16_t typeId,
                                                std::unique_ptr<BlockEntityRenderer> renderer) {
        if (typeId >= m_renderers.size()) {
            Log::Error("[BERDispatcher] typeId %u out of range", static_cast<unsigned>(typeId));
            return;
        }
        if (m_renderers[typeId]) {
            Log::Warning("[BERDispatcher] replacing renderer for typeId %u", static_cast<unsigned>(typeId));
        }
        m_renderers[typeId] = std::move(renderer);
    }

    BlockEntityRenderer* BlockEntityRenderDispatcher::GetRenderer(uint16_t typeId) const {
        if (typeId >= m_renderers.size()) return nullptr;
        return m_renderers[typeId].get();
    }

    void BlockEntityRenderDispatcher::CollectVisibleChunks(
            Client::ClientChunkManager* chunkMgr,
            std::vector<Client::ClientChunk*>& out,
            ChunkSet& seen) {
        out.clear();
        seen.clear();
        if (!chunkMgr) return;

        const ChunkRenderer* renderer = g_chunkRenderer;
        if (!renderer) {
            // No draw list to read — every loaded chunk, as before.
            std::vector<std::pair<Game::Math::ChunkPos, Client::ClientChunk*>> snap;
            chunkMgr->SnapshotLoadedChunks(snap);
            out.reserve(snap.size());
            for (auto& [pos, chunk] : snap) {
                if (chunk) out.push_back(chunk);
            }
            return;
        }

        // The draw list is per SECTION and a chunk has up to 24 of them in
        // it; the set collapses those to one chunk entry each. GetChunk is
        // a main-thread map lookup — cheap next to the BE-map walk it
        // replaces, and it answers the load state too (a chunk can drop
        // out between the BFS snapshot and this frame).
        // The CURRENT view's sections: inside a portal view this is the
        // portal's list, and the chests it looks at are gathered — the main
        // view's snapshot held nothing of what the portal showed.
        for (const SectionRenderData& rd : renderer->GetVisibleSections()) {
            if (!seen.insert(rd.chunkPos).second) continue;
            Client::ClientChunk* chunk = chunkMgr->GetChunk(rd.chunkPos);
            if (chunk && chunk->IsLoaded()) out.push_back(chunk);
        }
    }

    void BlockEntityRenderDispatcher::RenderAll(Client::ClientChunkManager* chunkMgr,
                                                 const glm::mat4& proj,
                                                 const glm::mat4& view,
                                                 const glm::vec3& cameraPos,
                                                 float partialTick) {
        PROFILE_ZONE_N("BlockEntityRenderAll");
        if (!chunkMgr) return;

        CollectVisibleChunks(chunkMgr, m_visibleChunks, m_seen);
        const ChunkRenderer* renderer = g_chunkRenderer;

        for (Client::ClientChunk* clientChunk : m_visibleChunks) {
            if (!clientChunk || !clientChunk->chunkData) continue;
            const auto& chunkBEs = clientChunk->chunkData->GetAllBlockEntities();
            if (chunkBEs.empty()) continue;   // most chunks, every frame

            for (const auto& [localPos, be] : chunkBEs) {
                if (!be) continue;

                // The map is keyed (localX, worldY, localZ): the section is
                // the world Y's. MC's extractVisibleBlockEntities never sees
                // a BE whose section is not in visibleSections; this is the
                // same gate, one hash lookup, against the same list the
                // chunk pass just drew from.
                if (renderer) {
                    const int sectionY = (localPos.y - Config::MinY) >> 4;
                    if (!renderer->IsSectionVisible(clientChunk->position, sectionY)) continue;
                }

                const auto* type = be->GetType();
                if (!type) continue;
                BlockEntityRenderer* beRenderer = GetRenderer(type->TypeId());
                if (!beRenderer) continue;

                // Distance cull (simple sphere; renderer can ask for more
                // range via GetViewDistance()).
                const glm::vec3 beCenter = glm::vec3(be->GetWorldPos()) + glm::vec3(0.5f);
                const float dx = beCenter.x - cameraPos.x;
                const float dz = beCenter.z - cameraPos.z;
                const float r  = static_cast<float>(beRenderer->GetViewDistance());
                if (dx * dx + dz * dz > r * r) continue;

                beRenderer->Render(*be, partialTick, proj, view, cameraPos);
            }
        }
    }

} // namespace Render
