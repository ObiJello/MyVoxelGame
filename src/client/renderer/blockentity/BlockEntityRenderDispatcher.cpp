// File: src/client/renderer/blockentity/BlockEntityRenderDispatcher.cpp
#include "BlockEntityRenderDispatcher.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/core/Log.hpp"
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

    void BlockEntityRenderDispatcher::RenderAll(Client::ClientChunkManager* chunkMgr,
                                                 const glm::mat4& proj,
                                                 const glm::mat4& view,
                                                 const glm::vec3& cameraPos,
                                                 float partialTick) {
        if (!chunkMgr) return;

        // Snapshot to avoid iterating the chunk map while another thread
        // mutates it (chunk packets arrive on the network thread). Mirrors
        // how ChunkRenderer's snapshot iteration works.
        std::vector<std::pair<Game::Math::ChunkPos, Client::ClientChunk*>> snap;
        chunkMgr->SnapshotLoadedChunks(snap);

        for (auto& [pos, clientChunk] : snap) {
            if (!clientChunk || !clientChunk->chunkData) continue;
            auto& chunkBEs = clientChunk->chunkData->MutableBlockEntities();
            for (auto& [localPos, be] : chunkBEs) {
                if (!be) continue;
                const auto* type = be->GetType();
                if (!type) continue;
                BlockEntityRenderer* renderer = GetRenderer(type->TypeId());
                if (!renderer) continue;

                // Distance cull (simple sphere; renderer can ask for more
                // range via GetViewDistance()).
                const glm::vec3 beCenter = glm::vec3(be->GetWorldPos()) + glm::vec3(0.5f);
                const float dx = beCenter.x - cameraPos.x;
                const float dz = beCenter.z - cameraPos.z;
                const float r  = static_cast<float>(renderer->GetViewDistance());
                if (dx * dx + dz * dz > r * r) continue;

                renderer->Render(*be, partialTick, proj, view, cameraPos);
            }
        }
    }

} // namespace Render
