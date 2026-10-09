// File: src/client/renderer/blockentity/BlockEntityRenderDispatcher.cpp
#include "BlockEntityRenderDispatcher.hpp"
#include "client/renderer/entity/EntityFrame.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../effects/VolumetricBeam.hpp"
#include "common/core/Config.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/chunk/Chunk.hpp"
#include <algorithm>
#include <cmath>
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

        m_offScreenReach = 0;
        for (const auto& r : m_renderers) {
            if (r && r->ShouldRenderOffScreen()) {
                m_offScreenReach = std::max(m_offScreenReach, r->GetOffScreenReach());
            }
        }
    }

    BlockEntityRenderer* BlockEntityRenderDispatcher::GetRenderer(uint16_t typeId) const {
        if (typeId >= m_renderers.size()) return nullptr;
        return m_renderers[typeId].get();
    }

    BlockEntityRenderDispatcher::VisibleChunkCache BlockEntityRenderDispatcher::s_visibleChunks;

    const std::vector<Client::ClientChunk*>& BlockEntityRenderDispatcher::CollectVisibleChunks(
            Client::ClientChunkManager* chunkMgr) {
        VisibleChunkCache& cache = s_visibleChunks;
        cache.valid = false;
        cache.chunks.clear();
        if (!chunkMgr) return cache.chunks;

        const ChunkRenderer* renderer = g_chunkRenderer;
        if (!renderer) {
            // No draw list to read — every loaded chunk, as before (and not
            // cached: nothing says when that set changes).
            std::vector<std::pair<Game::Math::ChunkPos, Client::ClientChunk*>> snap;
            chunkMgr->SnapshotLoadedChunks(snap);
            cache.chunks.reserve(snap.size());
            for (auto& [pos, chunk] : snap) {
                if (chunk) cache.chunks.push_back(chunk);
            }
            return cache.chunks;
        }

        // The CURRENT view's columns: inside a portal view this is the
        // portal's list, and the chests it looks at are gathered — the main
        // view's snapshot held nothing of what the portal showed. The same
        // pass of the same frame answers from the cache (a chunk unloads
        // between frames, never between the walkers of one pass). GetChunk
        // is a main-thread map lookup per column and answers the load state
        // too (a chunk can drop out between the BFS snapshot and this frame).
        const uint64_t pass = renderer->VisiblePassSerial();
        const uint32_t frame = EntityFrame::Serial();
        if (cache.valid && cache.passSerial == pass && cache.frameSerial == frame && cache.chunkMgr == chunkMgr) {
            return cache.chunks;
        }
        for (const Game::Math::ChunkPos& pos : renderer->GetVisibleChunkPositions()) {
            Client::ClientChunk* chunk = chunkMgr->GetChunk(pos);
            if (chunk && chunk->IsLoaded()) cache.chunks.push_back(chunk);
        }
        cache.passSerial = pass;
        cache.frameSerial = frame;
        cache.chunkMgr = chunkMgr;
        cache.valid = true;
        return cache.chunks;
    }

    void BlockEntityRenderDispatcher::RenderAll(Client::ClientChunkManager* chunkMgr,
                                                 const glm::mat4& proj,
                                                 const glm::mat4& view,
                                                 const glm::vec3& cameraPos,
                                                 float partialTick) {
        PROFILE_ZONE_N("BlockEntityRenderAll");
        if (!chunkMgr) return;
        {
            PROFILE_ZONE_N("BE.Retire");
            chunkMgr->RetireLandedBlockEntities();
        }
        // A new view for the volumetric beams (their depth snapshot is per
        // view): main, portal and panorama views all come through here.
        {
            PROFILE_ZONE_N("BE.BeamView");
            VolumetricBeam::Get().BeginView();
        }

        const std::vector<Client::ClientChunk*>& visibleChunks = [&]() -> const std::vector<Client::ClientChunk*>& {
            PROFILE_ZONE_N("BE.Collect");
            return CollectVisibleChunks(chunkMgr);
        }();
        const ChunkRenderer* renderer = g_chunkRenderer;

        {
        PROFILE_ZONE_N("BE.Walk");
        for (Client::ClientChunk* clientChunk : visibleChunks) {
            if (!clientChunk || !clientChunk->chunkData) continue;
            const auto& chunkBEs = clientChunk->chunkData->GetAllBlockEntities();
            if (chunkBEs.empty()) continue;   // most chunks, every frame

            for (const auto& [localPos, be] : chunkBEs) {
                if (!be) continue;

                const auto* type = be->GetType();
                if (!type) continue;
                BlockEntityRenderer* beRenderer = GetRenderer(type->TypeId());
                if (!beRenderer) continue;

                // The map is keyed (localX, worldY, localZ): the section is
                // the world Y's. MC's extractVisibleBlockEntities never sees
                // a BE whose section is not in visibleSections; this is the
                // same gate, one hash lookup, against the same list the
                // chunk pass just drew from. An off-screen renderer's
                // entities are MC's globalBlockEntities and skip it.
                if (renderer && !beRenderer->ShouldRenderOffScreen()) {
                    const int sectionY = (localPos.y - Config::MinY) >> 4;
                    if (!renderer->IsSectionVisible(clientChunk->position, sectionY)) continue;
                }

                // Distance cull (simple sphere; renderer can ask for more
                // range via GetViewDistance()).
                const glm::vec3 beCenter = glm::vec3(be->GetWorldPos()) + glm::vec3(0.5f);
                const float dx = beCenter.x - cameraPos.x;
                const float dz = beCenter.z - cameraPos.z;
                const float r  = static_cast<float>(beRenderer->GetViewDistance());
                if (dx * dx + dz * dz > r * r) continue;

                PROFILE_ZONE_N("BE.Render");
                beRenderer->Render(*be, partialTick, proj, view, cameraPos);
            }
        }
        }

        // With no chunk renderer CollectVisibleChunks already returned every
        // loaded chunk, so there is nothing left for the off-screen pass.
        if (renderer && m_offScreenReach > 0) {
            RenderOffScreen(chunkMgr, proj, view, cameraPos, partialTick);
        }
    }

    void BlockEntityRenderDispatcher::RenderOffScreen(Client::ClientChunkManager* chunkMgr,
                                                      const glm::mat4& proj,
                                                      const glm::mat4& view,
                                                      const glm::vec3& cameraPos,
                                                      float partialTick) {
        PROFILE_ZONE_N("BlockEntityRenderOffScreen");
        const ChunkRenderer* renderer = g_chunkRenderer;
        if (!renderer) return;
        // Bounded by the reach, not the render distance: a lamp whose own
        // section is on screen was drawn above at any distance; only one
        // near enough for its geometry to swing into view while its section
        // is behind the camera needs this pass. At the lighthouse's 72
        // blocks that is at most 11x11 chunk lookups a view.
        const float reach = static_cast<float>(m_offScreenReach);
        const int minCx = static_cast<int>(std::floor((cameraPos.x - reach) / 16.0f));
        const int maxCx = static_cast<int>(std::floor((cameraPos.x + reach) / 16.0f));
        const int minCz = static_cast<int>(std::floor((cameraPos.z - reach) / 16.0f));
        const int maxCz = static_cast<int>(std::floor((cameraPos.z + reach) / 16.0f));

        for (int cz = minCz; cz <= maxCz; ++cz) {
            for (int cx = minCx; cx <= maxCx; ++cx) {
                const Game::Math::ChunkPos pos{cx, cz};
                if (renderer->IsColumnVisible(pos)) continue;   // walked by the visible pass
                Client::ClientChunk* clientChunk = chunkMgr->GetChunk(pos);
                if (!clientChunk || !clientChunk->IsLoaded() || !clientChunk->chunkData) continue;
                const auto& chunkBEs = clientChunk->chunkData->GetAllBlockEntities();
                if (chunkBEs.empty()) continue;

                for (const auto& [localPos, be] : chunkBEs) {
                    if (!be) continue;
                    const auto* type = be->GetType();
                    if (!type) continue;
                    BlockEntityRenderer* beRenderer = GetRenderer(type->TypeId());
                    if (!beRenderer || !beRenderer->ShouldRenderOffScreen()) continue;

                    const glm::vec3 beCenter = glm::vec3(be->GetWorldPos()) + glm::vec3(0.5f);
                    const float dx = beCenter.x - cameraPos.x;
                    const float dz = beCenter.z - cameraPos.z;
                    const float r  = static_cast<float>(beRenderer->GetOffScreenReach());
                    if (dx * dx + dz * dz > r * r) continue;

                    beRenderer->Render(*be, partialTick, proj, view, cameraPos);
                }
            }
        }
    }

} // namespace Render
