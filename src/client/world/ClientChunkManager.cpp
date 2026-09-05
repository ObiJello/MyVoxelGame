// File: src/client/world/ClientChunkManager.cpp
#include <cstdint>
#include "ClientChunkManager.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "ClientWorkerPool.hpp"
#include "../renderer/core/Frustum.hpp"
#include "../renderer/mesh/MeshJobData.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "../renderer/mesh/ChunkRenderer.hpp"
#include "../renderer/mesh/Mesher.hpp"   // GreedyPaletteGen (debug-palette staleness)
#include "platform/GameDirectory.hpp"
#include "common/core/HardwareProfile.hpp"
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "../portal/ClientImmersivePortals.hpp"
#endif
#include <glad/glad.h>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>

namespace Client {

    // Bound-level pointer, owned by ClientLevel (see ClientLevel.hpp).
    ClientChunkManager* g_clientChunkManager = nullptr;


    size_t ClientChunkManager::ComputeRetainBudgetBytes() {
        // 1.5 GB is the tuned value from the M-series machines the streaming
        // work was measured on (16 GB+); below that, a tenth of RAM. MC's
        // equivalent knobs scale from memory the same way (the render-
        // distance ceiling on maxMemory, SectionBufferBuilderPool's
        // maxMemory * 0.3). Floored so a tiny machine still gets the
        // instant-revisit benefit for the chunks right around the player.
        constexpr size_t kMaxBudget = size_t(1536) << 20;   // 1.5 GB
        constexpr size_t kMinBudget = size_t(128)  << 20;   // 128 MB
        const uint64_t ram = Core::HardwareProfile::Get().physicalMemoryBytes;
        if (ram == 0) return kMaxBudget;   // detection failed: the old constant
        const size_t tenth = static_cast<size_t>(ram / 10);
        const size_t budget = std::clamp(tenth, kMinBudget, kMaxBudget);
        Log::Info("Client chunk retention budget: %zu MB (RAM %llu MB)",
                  budget >> 20, static_cast<unsigned long long>(ram >> 20));
        return budget;
    }

    ClientChunkManager::ClientChunkManager() {
        m_pendingDiffs = std::make_unique<PendingDiffsManager>();
        Log::Info("ClientChunkManager created with PendingDiffsManager");
    }

    ClientChunkManager::~ClientChunkManager() {
        Shutdown();
        Log::Info("ClientChunkManager destroyed");
    }

    void ClientChunkManager::Initialize() {
        ASSERT_MAIN_THREAD();
        Log::Info("Initializing ClientChunkManager...");
        
        // Clear any existing data
        m_chunks.clear();
        m_loadedChunkCount = 0;
        
        // Reset pending diffs
        if (m_pendingDiffs) {
            m_pendingDiffs->ClearAll();
        }
        
        // Reset generation counter
        m_nextGeneration = 1;

        // Bind the prediction handler's world hooks (MC ClientLevel owns the
        // equivalent handler and calls straight into its own setBlock).
        m_prediction.Clear();
        m_prediction.SetHooks(
            [this](const glm::ivec3& pos, Game::BlockID state, Game::BlockStateIndex stateIndex) {
                SetBlockLocal(pos, state, stateIndex);
            },
            [this](const glm::ivec3& pos) { return GetBlockAndStateAt(pos); });

        Log::Info("ClientChunkManager initialized successfully");
    }

    void ClientChunkManager::Shutdown() {
        ASSERT_MAIN_THREAD();
        Log::Info("Shutting down ClientChunkManager...");

        // Stale-border-mesh accounting for the session (see the counters in
        // the header): how many uploads were built with a neighbour chunk
        // missing, and how many sections had to be rebuilt because that
        // neighbour later turned out to exist.
        Log::Info("Border-mesh convergence: %zu meshes built with a missing "
                  "neighbour, %zu rebuilt after the neighbour arrived",
                  m_partialNeighborBuilds, m_staleBorderRemeshes);


        // Clear all chunks
        m_chunks.clear();
        m_loadedChunkCount = 0;
        
        // Clear pending diffs
        if (m_pendingDiffs) {
            m_pendingDiffs->ClearAll();
        }
        
        Log::Info("ClientChunkManager shutdown complete");
    }

    // ========================================================================
    // CHUNK STATE MANAGEMENT
    // ========================================================================

    
    void ClientChunkManager::UnloadChunk(Game::Math::ChunkPos chunkPos) {
        PROFILE_ZONE;
        ASSERT_MAIN_THREAD();
        Log::Debug("CLIENT UNLOAD: chunk (%d, %d)", chunkPos.x, chunkPos.z);

#if ENABLE_IMMERSIVE_PORTALS
        // A portal lives in its origin chunk: it leaves with it (and comes
        // back with the chunk's next send, retained or not).
        GetClientImmersivePortals().OnChunkUnloaded(chunkPos);
#endif

        // Mark neighbor chunks' sections as dirty BEFORE unloading
        // This ensures they rebuild their meshes to show previously culled faces
        {
            PROFILE_ZONE_N("Unload.NeighborsDirty");
            MarkNeighborSectionsDirty(chunkPos);
        }
        
        // Cancel in-flight mesh tasks per-section (Minecraft-style per-task cancellation)
        {
            PROFILE_ZONE_N("Unload.CancelJobs");
            auto chunkIt = m_chunks.find(chunkPos);
            if (chunkIt != m_chunks.end()) {
                for (int sy = 0; sy < 24; ++sy) {
                    auto& si = chunkIt->second->sectionInfos[sy];
                    if (si.lastMeshJob) {
                        si.lastMeshJob->Cancel();
                        si.lastMeshJob.reset();
                    }
                }
            }
        }
        
        // Drop any pending diffs for this chunk
        if (m_pendingDiffs) {
            PROFILE_ZONE_N("Unload.Diffs");
            m_pendingDiffs->DropChunkDiffs(chunkPos);
        }

        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end() && it->second->chunkData && m_meshes) {
            // Park instead of free (retention cache, see RestoreRetainedChunk).
            PROFILE_ZONE_N("Unload.Park");
            std::unique_ptr<ClientChunk> chunk = std::move(it->second);
            m_chunks.erase(it);
            m_chunksWithDirtySections.erase(chunkPos);
            TransitionChunkState(chunk.get(), ChunkState::UNLOADED);
            size_t bytes = m_meshes->ParkChunkGPUData(chunkPos);
            for (int sy = 0; sy < Game::Math::SECTIONS_PER_CHUNK; ++sy) {
                if (const auto* sec = chunk->chunkData->GetSection(sy)) {
                    bytes += sec->States().RawWords().size() * 8 + sec->Biomes().RawWords().size() * 8;
                }
            }
            bytes += sizeof(ClientChunk) + sizeof(Game::Chunk);
            if (m_retained.count(chunkPos)) DiscardRetained(chunkPos);
            m_retainedLru.push_front(chunkPos);
            m_retained[chunkPos] = Retained{std::move(chunk), bytes, m_retainedLru.begin()};
            m_retainedBytes += bytes;
            while (m_retainedBytes > m_retainBudgetBytes && !m_retainedLru.empty()) {
                DiscardRetained(m_retainedLru.back());
            }
            return;
        }
        // Clean up GPU resources (vertex/index buffers) before erasing chunk data
        if (m_meshes) {
            PROFILE_ZONE_N("Unload.GPUData");
            m_meshes->RemoveChunkGPUData(chunkPos);
        }
        if (it != m_chunks.end()) {
            PROFILE_ZONE_N("Unload.Erase");
            if (it->second && it->second->state == ChunkState::LOADED && m_loadedChunkCount > 0) --m_loadedChunkCount;
            m_chunks.erase(it);
            Log::Debug("Unloaded chunk (%d, %d)", chunkPos.x, chunkPos.z);
        }
        m_chunksWithDirtySections.erase(chunkPos);
    }

    void ClientChunkManager::MarkSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY,
                                              bool fromPlayer) {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end() && it->second->state == ChunkState::LOADED) {
            // Increment version to trigger rebuild (Minecraft-style)
            auto& sectionInfo = it->second->sectionInfos[sectionY];
            sectionInfo.version++;
            sectionInfo.dirty = true;
            // Sticky until the section is scheduled — a player edit that lands
            // while an earlier compile is still in flight must not lose its
            // priority when the version bump re-dirties the section.
            if (fromPlayer) sectionInfo.dirtyFromPlayer = true;
            it->second->AddDirty(sectionY);
            m_chunksWithDirtySections.insert(chunkPos);
            m_schedulerSkip = 0;
            // Mass destruction: after enough section changes, force one
            // authoritative full BFS rebuild. The incremental path only ADDS
            // reachable sections; a blast that re-shapes the neighbourhood
            // needs the conservative pre-blast answer thrown away too, and
            // without this that only happened when the camera moved.
            // The section just became see-through to the BFS (dirty = open,
            // see SectionOcclusionGraph::BuildInput) — seed a propagation so
            // the partial update walks through it THIS frame instead of
            // waiting for a full rebuild.
            if (m_renderer) {
                m_renderer->SchedulePropagationFrom(chunkPos, sectionY);
            }
            if (++m_dirtyMarksSinceRebuild >= 256) {
                m_dirtyMarksSinceRebuild = 0;
                if (m_renderer) {
                    m_renderer->MarkVisibleSectionsDirty();
                }
            }
            Log::Debug("Marked chunk (%d, %d) section %d as dirty (version now %u)",
                      chunkPos.x, chunkPos.z, sectionY, sectionInfo.version);
        }
    }

    void ClientChunkManager::RefreshSectionEmptiness(ClientChunk& chunk, int sectionY) {
        ASSERT_MAIN_THREAD();
        if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) return;
        if (!chunk.chunkData) return;

        auto& sectionInfo = chunk.sectionInfos[sectionY];

        // Re-derive rather than assign `false` on a non-air write: this has to
        // handle the reverse too. Mining the last block out of a section makes
        // it all-air again, and a section left wrongly marked NON-air is
        // rendered as an opaque BFS blocker (visBits 0 while unmeshed), which
        // hides everything behind it. ChunkSection::IsAllAir is O(1) — a
        // single-value palette test — so this is cheap enough for every write,
        // which is the only discipline that keeps the mirror honest.
        const Game::ChunkSection* section = chunk.chunkData->GetSection(sectionY);
        const bool nowAllAir = (section == nullptr) || section->IsAllAir();
        if (nowAllAir == sectionInfo.isAllAir) return;

        sectionInfo.isAllAir = nowAllAir;
        // Same promotion as on load. Never cleared when a section stops being
        // air: MC's state machine only moves UNCOMPILED -> EMPTY -> compiled,
        // and a section that gains blocks is marked dirty below and will build.
        if (nowAllAir) sectionInfo.meshResolvedEmpty = true;

        // The cell's BFS inputs just changed — `renderable` flips, and so does
        // whether it counts as fully see-through. Seed a propagation so the
        // graph picks it up this frame instead of waiting for the next full
        // rebuild (which only happens on an 8-block camera move).
        //
        // This matters more than it looks: with the default chunk-builder mode
        // the visible list is the ONLY mesh-candidate source
        // (`needDirtyWalk` is false), so a section that is not in the list is
        // never even handed to the mesher. Until the graph learns the section
        // is renderable, marking it dirty accomplishes nothing.
        if (m_renderer) {
            m_renderer->SchedulePropagationFrom(chunk.position, sectionY);
        }
    }

    void ClientChunkManager::MarkChunkDirty(Game::Math::ChunkPos chunkPos) {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end() && it->second->state == ChunkState::LOADED) {
            // Mark all 24 sections as dirty and increment their versions (Minecraft-style)
            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                auto& sectionInfo = it->second->sectionInfos[sectionY];
                sectionInfo.version++;
                sectionInfo.dirty = true;
                it->second->AddDirty(sectionY);
            }
            m_chunksWithDirtySections.insert(chunkPos);
            m_schedulerSkip = 0;
            Log::Debug("Marked all sections in chunk (%d, %d) as dirty", chunkPos.x, chunkPos.z);
        }
    }
    
    void ClientChunkManager::ClearSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY) {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end()) {
            size_t removed = it->second->dirtySections.erase(sectionY);
            if (sectionY >= 0 && sectionY < 32) it->second->dirtyMask &= ~(1u << sectionY);
            if (removed > 0) {
                Log::Debug("Cleared dirty flag for chunk (%d, %d) section %d", 
                          chunkPos.x, chunkPos.z, sectionY);
            }
        }
    }
    
    void ClientChunkManager::MarkNeighborSectionsDirty(Game::Math::ChunkPos chunkPos) {
        PROFILE_ZONE;
        ASSERT_MAIN_THREAD();

        Log::Debug("=== MarkNeighborSectionsDirty for chunk (%d, %d) ===", chunkPos.x, chunkPos.z);

        // Get the source chunk's section info so we can skip neighbor sections
        // where the source section at the same Y level is all-air (no new blocks
        // to cull against means the neighbor's mesh won't change)
        auto sourceIt = m_chunks.find(chunkPos);
        const ClientChunk* sourceChunk = (sourceIt != m_chunks.end()) ? sourceIt->second.get() : nullptr;

        // Helper lambda: mark dirty only neighbor sections that are non-empty AND
        // where the source chunk's section at the same Y is also non-empty
        auto markNeighborDirty = [&](Game::Math::ChunkPos neighborPos, const char* dirLabel) {
            auto neighborIt = m_chunks.find(neighborPos);
            if (neighborIt == m_chunks.end() || neighborIt->second->state != ChunkState::LOADED) {
                // PARKED neighbours must be dirtied too. A chunk in the
                // retention cache is not in m_chunks, so it used to be skipped
                // outright — and then RestoreRetainedChunk revived meshes that
                // were built against blocks this chunk has since replaced (or
                // against a chunk that is now gone). The parked chunk keeps its
                // sectionInfos/dirtySections through the park, and
                // RestoreRetainedChunk re-registers any dirty sections with the
                // scheduler on revival, so marking here is enough to make the
                // revived meshes converge.
                auto parkedIt = m_retained.find(neighborPos);
                if (parkedIt != m_retained.end() && parkedIt->second.chunk) {
                    ClientChunk& parked = *parkedIt->second.chunk;
                    for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                        auto& si = parked.sectionInfos[sectionY];
                        if (si.isAllAir || si.dirty) continue;
                        if (sourceChunk && sourceChunk->sectionInfos[sectionY].isAllAir) continue;
                        si.version++;
                        si.dirty = true;
                        parked.AddDirty(sectionY);
                    }
                }
                return;
            }
            int dirtyCount = 0;
            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                auto& sectionInfo = neighborIt->second->sectionInfos[sectionY];
                // Skip if neighbor section is all-air (no geometry to remesh)
                if (sectionInfo.isAllAir) continue;
                // Skip if already dirty
                if (sectionInfo.dirty) continue;
                // Skip if the source chunk's section at this Y level is all-air:
                // no new blocks to cull against, so neighbor mesh won't change
                if (sourceChunk && sourceChunk->sectionInfos[sectionY].isAllAir) continue;

                sectionInfo.version++;
                sectionInfo.dirty = true;
                neighborIt->second->AddDirty(sectionY);
                dirtyCount++;
            }
            if (dirtyCount > 0) {
                m_chunksWithDirtySections.insert(neighborPos);
                Log::Debug("  %s neighbor (%d, %d): marked %d sections dirty",
                          dirLabel, neighborPos.x, neighborPos.z, dirtyCount);
            }
        };

        markNeighborDirty({chunkPos.x, chunkPos.z - 1}, "North");
        markNeighborDirty({chunkPos.x, chunkPos.z + 1}, "South");
        markNeighborDirty({chunkPos.x + 1, chunkPos.z},  "East");
        markNeighborDirty({chunkPos.x - 1, chunkPos.z},  "West");
    }

    // ========================================================================
    // CHUNK ACCESS
    // ========================================================================

    // Biome at an absolute world position, resolved through the chunk map so
    // it works across chunk borders. Returns 0 (the fallback biome) for a chunk
    // that is not loaded or carries no biome data.
    uint16_t ClientChunkManager::BiomeAtWorld(int worldX, int worldY, int worldZ) {
        const Game::Math::ChunkPos cp =
            Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        ClientChunk* c = GetChunk(cp);
        if (!c || !c->chunkData) return Game::kFallbackBiomeId;
        return c->chunkData->GetBiome(worldX - cp.x * Game::Math::CHUNK_SIZE_X,
                                      worldY,
                                      worldZ - cp.z * Game::Math::CHUNK_SIZE_Z);
    }

    ClientChunk* ClientChunkManager::GetChunk(Game::Math::ChunkPos chunkPos) {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end()) {
            // No access stamp. `lastAccessTime` was written here and read
            // NOWHERE in the tree — a dead field whose only effect was a
            // steady_clock::now() on the hottest call in the client.
            //
            // GetChunk backs every block query the client makes, so at a
            // hundred thousand primed TNT each doing collision reads it landed
            // at 851 of 6,090 samples on the main thread: 14% of the client,
            // and 57% of MoveApproximate, spent in mach_continuous_time.
            // A `sample` profile found it; no zone would have, because it sat
            // inside an accessor nobody would think to instrument.
            return it->second.get();
        }
        return nullptr;
    }

    const ClientChunk* ClientChunkManager::GetChunk(Game::Math::ChunkPos chunkPos) const {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        return (it != m_chunks.end()) ? it->second.get() : nullptr;
    }
    
    SectionInfo* ClientChunkManager::GetSectionInfo(Game::Math::ChunkPos chunkPos, int sectionY) {
        // No mutex needed - called from render thread only
        if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) {
            return nullptr;
        }
        
        auto* chunk = GetChunk(chunkPos);
        if (!chunk || chunk->state != ChunkState::LOADED) {
            return nullptr;
        }
        
        return &chunk->sectionInfos[sectionY];
    }
    
    const SectionInfo* ClientChunkManager::GetSectionInfo(Game::Math::ChunkPos chunkPos, int sectionY) const {
        // No mutex needed - called from render thread only
        if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) {
            return nullptr;
        }
        
        auto* chunk = GetChunk(chunkPos);
        if (!chunk || chunk->state != ChunkState::LOADED) {
            return nullptr;
        }
        
        return &chunk->sectionInfos[sectionY];
    }

    ChunkState ClientChunkManager::GetChunkState(Game::Math::ChunkPos chunkPos) const {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        return (it != m_chunks.end()) ? it->second->state : ChunkState::UNLOADED;
    }

    bool ClientChunkManager::IsChunkLoaded(Game::Math::ChunkPos chunkPos) const {
        ChunkState state = GetChunkState(chunkPos);
        return state == ChunkState::LOADED;
    }

    // ========================================================================
    // STATISTICS
    // ========================================================================
    
    size_t ClientChunkManager::GetLoadedChunkCount() const {
        return m_loadedChunkCount;
    }
    
    void ClientChunkManager::GetSectionStats(size_t& totalSections, size_t& readySections, 
                                            size_t& meshingSections, size_t& dirtySections) const {
        totalSections = 0;
        readySections = 0;
        meshingSections = 0;
        dirtySections = 0;
        
        for (const auto& [pos, chunk] : m_chunks) {
            if (chunk && chunk->state == ChunkState::LOADED) {
                for (int sectionY = 0; sectionY < 24; ++sectionY) {
                    const auto& sectionInfo = chunk->sectionInfos[sectionY];
                    if (sectionInfo.hasCpuData) {
                        totalSections++;
                        
                        if (sectionInfo.state == SectionState::READY) {
                            readySections++;
                        } else if (sectionInfo.state == SectionState::MESHING) {
                            meshingSections++;
                        }
                        
                        if (sectionInfo.dirty) {
                            dirtySections++;
                        }
                    }
                }
            }
        }
    }

    // ========================================================================
    // INTERNAL METHODS
    // ========================================================================

    void ClientChunkManager::TransitionChunkState(ClientChunk* chunk, ChunkState newState) {
        if (!chunk) {
            return;
        }
        
        ChunkState oldState = chunk->state;
        chunk->state = newState;
        
        // Notify RenderGrid when chunk becomes loaded
        if (newState == ChunkState::LOADED && oldState != ChunkState::LOADED) {
            ++m_loadedChunkCount;
            NotifyRenderGridChunkLoaded(chunk->position, chunk);
            chunk->neighborsAllLoaded = -1;
            InvalidateNeighborLoadCache(chunk->position);
        }
        // Notify RenderGrid when chunk becomes unloaded
        else if (oldState == ChunkState::LOADED && newState != ChunkState::LOADED) {
            if (m_loadedChunkCount > 0) --m_loadedChunkCount;
            NotifyRenderGridChunkUnloaded(chunk->position);
            chunk->neighborsAllLoaded = -1;
            InvalidateNeighborLoadCache(chunk->position);
        }
    }

    bool ClientChunkManager::NeighborsAllLoadedCached(ClientChunk& chunk) {
        if (chunk.neighborsAllLoaded < 0) {
            chunk.neighborsAllLoaded = HasAllNeighborChunks(chunk.position) ? 1 : 0;
        }
        return chunk.neighborsAllLoaded == 1;
    }

    void ClientChunkManager::InvalidateNeighborLoadCache(Game::Math::ChunkPos pos) {
        static constexpr int kDX[8] = { -1, 0, 1, 0, -1, -1, 1, 1 };
        static constexpr int kDZ[8] = { 0, -1, 0, 1, -1, 1, -1, 1 };
        for (int i = 0; i < 8; ++i) {
            auto it = m_chunks.find(Game::Math::ChunkPos{pos.x + kDX[i], pos.z + kDZ[i]});
            if (it != m_chunks.end() && it->second) it->second->neighborsAllLoaded = -1;
        }
    }


    // ========================================================================
    // GLOBAL FUNCTIONS
    // ========================================================================

    ClientChunk* GetClientChunk(Game::Math::ChunkPos chunkPos) {
        return g_clientChunkManager ? g_clientChunkManager->GetChunk(chunkPos) : nullptr;
    }

    ChunkState GetClientChunkState(Game::Math::ChunkPos chunkPos) {
        return g_clientChunkManager ? g_clientChunkManager->GetChunkState(chunkPos) : ChunkState::UNLOADED;
    }

    bool IsClientChunkLoaded(Game::Math::ChunkPos chunkPos) {
        return g_clientChunkManager ? g_clientChunkManager->IsChunkLoaded(chunkPos) : false;
    }

    // Process ChunkDataS2CPacket (new format)
    std::shared_ptr<Game::Chunk> ClientChunkManager::PrebuildChunk(const Network::ChunkDataS2CPacket& packet) {
        PROFILE_ZONE_N("PrebuildChunk");
        auto built = std::make_shared<Game::Chunk>();
        for (int y = 0; y < Game::Math::SECTIONS_PER_CHUNK &&
                        y < static_cast<int>(packet.sections.size()); ++y) {
            auto* section = built->GetSection(y);
            if (!section) continue;
            auto& mutableSection = const_cast<Network::ChunkDataS2CPacket::SectionData&>(packet.sections[y]);
            Game::PalettedContainer states(
                Game::PaletteStrategy::ForBlockStates(Game::kBlockStateBits),
                Game::BlockState{}.RawId());
            Game::PalettedContainer biomes = Game::ChunkSection::MakeBiomeContainer();
            const bool okStates = states.ReadFrom(mutableSection.states.bits,
                std::move(mutableSection.states.palette), std::move(mutableSection.states.words));
            const bool okBiomes = biomes.ReadFrom(mutableSection.biomes.bits,
                std::move(mutableSection.biomes.palette), std::move(mutableSection.biomes.words));
            if (!okStates || !okBiomes) {
                Log::Warning("Failed to decode section %d for chunk (%d, %d)", y, packet.chunkX, packet.chunkZ);
                continue;
            }
            section->AdoptStates(std::move(states));
            section->AdoptBiomes(std::move(biomes));
        }
        return built;
    }

    void ClientChunkManager::ProcessChunkDataS2CPacket(const Network::ChunkDataS2CPacket& packet) {
        PROFILE_ZONE_N("ProcessChunkData");
        ASSERT_MAIN_THREAD();

        Game::Math::ChunkPos chunkPos{packet.chunkX, packet.chunkZ};
        ApplyChunkData(chunkPos, packet);
    }
    
    // Process block change packet
    void ClientChunkManager::ProcessBlockChange(const Network::BlockChangeS2CPacket& packet) {
        ASSERT_MAIN_THREAD();
        const glm::ivec3 pos{packet.worldX, packet.worldY, packet.worldZ};

        // MC ClientLevel.setServerVerifiedBlockState: if we have an
        // outstanding prediction at this position, the server's state is filed
        // against that prediction instead of being written now. Writing it
        // would undo the player's predicted block for the rest of the round
        // trip — the exact flicker prediction exists to avoid.
        if (m_prediction.RecordServerState(pos, packet.newBlockId, packet.newBlockState)) {
            return;
        }

        // Calculate chunk position from world coordinates
        Game::Math::ChunkPos chunkPos{
            packet.worldX >> 4,  // Divide by 16
            packet.worldZ >> 4
        };

        // Get the chunk
        ClientChunk* chunk = GetChunk(chunkPos);
        if (!chunk || !chunk->chunkData) {
            // Chunk not loaded yet - add to pending diffs
            Log::Debug("Chunk (%d, %d) not loaded - adding block change to pending diffs",
                      chunkPos.x, chunkPos.z);
            if (m_pendingDiffs) {
                m_pendingDiffs->AddBlockChange(chunkPos, packet);
            }
            return;
        }

        SetBlockLocal(pos, packet.newBlockId, packet.newBlockState);
    }

    void ClientChunkManager::SetBlockLocal(const glm::ivec3& pos, Game::BlockID blockId,
                                           Game::BlockStateIndex stateIndex, bool fromPlayer) {
        ASSERT_MAIN_THREAD();
        Game::Math::ChunkPos chunkPos{pos.x >> 4, pos.z >> 4};

        ClientChunk* chunk = GetChunk(chunkPos);
        if (!chunk || !chunk->chunkData) return;

        // Calculate local block position within chunk
        int localX = pos.x & 0xF;  // mod 16
        int localZ = pos.z & 0xF;
        int sectionY = (pos.y + 64) >> 4;  // Convert to section index

        // Read the outgoing block BEFORE the write — the end-portal index has
        // to know whether a portal is being removed, and there is no other
        // record of what used to be here.
        const Game::BlockID prevBlockId =
            chunk->chunkData->GetBlock(localX, pos.y, localZ);

        // Set the block in the chunk
        chunk->chunkData->SetBlock(localX, pos.y, localZ, blockId, stateIndex);

        // Keep the render-side emptiness mirror true. This is the whole reason
        // a block placed into a section that was air at chunk-load time used to
        // be solid and invisible — see RefreshSectionEmptiness.
        RefreshSectionEmptiness(*chunk, sectionY);

        // Patch ClientChunk::endPortals in place rather than rescanning. Every
        // ordinary edit costs the two comparisons below and nothing else; only
        // an edit that actually involves a portal touches the vector.
        if (prevBlockId == Game::BlockID::EndPortal && blockId != Game::BlockID::EndPortal) {
            auto& list = chunk->endPortals;
            list.erase(std::remove(list.begin(), list.end(), pos), list.end());
        } else if (prevBlockId != Game::BlockID::EndPortal && blockId == Game::BlockID::EndPortal) {
            chunk->endPortals.push_back(pos);
        }
        if (prevBlockId == Game::BlockID::EndGateway && blockId != Game::BlockID::EndGateway) {
            auto& list = chunk->endGateways;
            list.erase(std::remove(list.begin(), list.end(), pos), list.end());
        } else if (prevBlockId != Game::BlockID::EndGateway && blockId == Game::BlockID::EndGateway) {
            chunk->endGateways.push_back(pos);
        }

        // Mark section as dirty for remeshing
        MarkSectionDirty(chunkPos, sectionY, fromPlayer);

        // Mark neighbor sections dirty when block is on a chunk/section boundary
        // (the neighbor's mesh depends on this block for face culling).
        // fromPlayer propagates to them too — MC's setBlocksDirty carries the
        // player flag across the whole dirtied range, and a boundary edit whose
        // neighbour lagged a frame behind would look like a seam.
        int localY = (pos.y + 64) & 0xF;  // position within section (0-15)
        if (localX == 0)  MarkSectionDirty({chunkPos.x - 1, chunkPos.z}, sectionY, fromPlayer);
        if (localX == 15) MarkSectionDirty({chunkPos.x + 1, chunkPos.z}, sectionY, fromPlayer);
        if (localZ == 0)  MarkSectionDirty({chunkPos.x, chunkPos.z - 1}, sectionY, fromPlayer);
        if (localZ == 15) MarkSectionDirty({chunkPos.x, chunkPos.z + 1}, sectionY, fromPlayer);
        if (localY == 0  && sectionY > 0)  MarkSectionDirty(chunkPos, sectionY - 1, fromPlayer);
        if (localY == 15 && sectionY < 23) MarkSectionDirty(chunkPos, sectionY + 1, fromPlayer);
    }

    Game::BlockID ClientChunkManager::GetBlockAt(const glm::ivec3& pos) const {
        const ClientChunk* chunk = GetChunk({pos.x >> 4, pos.z >> 4});
        if (!chunk || !chunk->chunkData) return Game::BlockID::Air;
        return chunk->chunkData->GetBlock(pos.x & 0xF, pos.y, pos.z & 0xF);
    }

    std::pair<Game::BlockID, Game::BlockStateIndex> ClientChunkManager::GetBlockAndStateAt(const glm::ivec3& pos) const {
        const ClientChunk* chunk = GetChunk({pos.x >> 4, pos.z >> 4});
        if (!chunk || !chunk->chunkData) return {Game::BlockID::Air, 0};
        const int lx = pos.x & 0xF, lz = pos.z & 0xF;
        return { chunk->chunkData->GetBlock(lx, pos.y, lz),
                 chunk->chunkData->GetBlockState(lx, pos.y, lz) };
    }

    void ClientChunkManager::PredictBlockChange(const glm::ivec3& pos,
                                                Game::BlockID newBlock,
                                                uint32_t sequence,
                                                Game::BlockStateIndex stateIndex) {
        ASSERT_MAIN_THREAD();
        // Only predict into loaded chunks. Predicting into a chunk we don't
        // have would leave a record whose rollback target is a fabricated Air.
        ClientChunk* chunk = GetChunk({pos.x >> 4, pos.z >> 4});
        if (!chunk || !chunk->chunkData) return;

        // Retain BEFORE writing — the retained state is what the server still
        // believes is there, which is exactly the pre-write block AND its state.
        const auto [prevBlock, prevState] = GetBlockAndStateAt(pos);
        m_prediction.Retain(pos, prevBlock, prevState, sequence);
        // THIS client placed/broke the block — MC's isDirtyFromPlayer. The
        // server-echo path (HandleBlockChange) and the prediction-rollback hook
        // deliberately do not set it: those are corrections, not player intent,
        // and a rollback compiling synchronously would stutter on packet loss.
        SetBlockLocal(pos, newBlock, stateIndex, /*fromPlayer=*/true);
    }

    void ClientChunkManager::HandleBlockChangedAck(uint32_t sequence) {
        ASSERT_MAIN_THREAD();
        m_prediction.EndPredictionsUpTo(sequence);
    }
    
    void ClientChunkManager::ApplyChunkData(Game::Math::ChunkPos chunkPos, const Network::ChunkDataS2CPacket& packet) {
        PROFILE_ZONE_N("ApplyChunkData");
        ASSERT_MAIN_THREAD();
        
        // Get or create client chunk
        if (m_retained.count(chunkPos)) DiscardRetained(chunkPos);   // full data supersedes the parked copy
        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end()) {
            auto clientChunk = std::make_unique<ClientChunk>(chunkPos);
            it = m_chunks.emplace(chunkPos, std::move(clientChunk)).first;
        }
        
        ClientChunk* chunk = it->second.get();
        
        // Increment generation for groundUp loads
        if (packet.groundUpContinuous) {
            chunk->generation = m_nextGeneration.fetch_add(1);
            Log::Debug("Chunk (%d, %d) groundUp load - generation %u", 
                      chunkPos.x, chunkPos.z, chunk->generation);
        }
        
        // Create or replace chunk data

        
        // Initialize ALL sections first (for ground-up loads)
        if (packet.groundUpContinuous) {
            for (int y = 0; y < Game::Math::SECTIONS_PER_CHUNK; ++y) {
                auto& sectionInfo = chunk->sectionInfos[y];
                // Reset to default state for ground-up load
                sectionInfo.hasCpuData = true;  // We'll know the state after parsing
                sectionInfo.isAllAir = true;    // Assume air until proven otherwise
                // Not resolved yet either — "assume air" is a placeholder, and
                // promoting on it would let the load tracker call a section
                // ready before its blocks have been parsed.
                sectionInfo.meshResolvedEmpty = false;
                sectionInfo.state = SectionState::LOADED;
                // version is NOT reset. On a re-send of a chunk that is
                // already loaded, zeroing it made every future job's
                // generation smaller than uploadedVersion, so AcceptMeshResult
                // dropped every new mesh as Drop_Replaced FOREVER and the
                // section kept the pre-reload geometry. Versions must stay
                // monotonic for the section's whole client lifetime; the
                // per-section ++ below is what marks the new content.
                sectionInfo.dirty = false;
                sectionInfo.meshingVersion = 0;
                // Any in-flight job snapshotted the PREVIOUS content; its
                // result is wasted work at best, so stop it early.
                if (sectionInfo.lastMeshJob) {
                    sectionInfo.lastMeshJob->Cancel();
                    sectionInfo.lastMeshJob.reset();
                }
            }
        }
        
        // Apply section data.
        //
        // Positional and total: `sections` is exactly SECTIONS_PER_CHUNK long
        // (the deserializer resizes to it), section Y is the index, and there
        // is no bitmask to consult — MC LevelChunk.replaceWithPacketData reads
        // the sections back in the same fixed order they were written.
        //
        // Every section is DECODED, including all-air ones. The guard here
        // used to be `if (section && !sectionData.IsEmpty())`, and IsEmpty was
        // `blockCount == 0` — so an all-air section's decode was skipped
        // wholesale, taking `AdoptBiomes` with it. That is precisely why the
        // sky had no biomes: the block data being empty says nothing about
        // whether the biome container is.
        // Adopt the chunk built on the I/O thread (or build it here if a
        // packet arrived without one), then derive the per-section bookkeeping.
        std::shared_ptr<Game::Chunk> built = packet.prebuilt ? packet.prebuilt : PrebuildChunk(packet);
        built->pos = chunkPos;
        built->modStamp.store(packet.modStamp, std::memory_order_relaxed);
        chunk->chunkData = built;
        for (int y = 0; y < Game::Math::SECTIONS_PER_CHUNK &&
                        y < static_cast<int>(packet.sections.size()); ++y) {
            auto* section = chunk->chunkData->GetSection(y);
            if (!section) continue;
            auto& sectionInfo = chunk->sectionInfos[y];
            sectionInfo.hasCpuData = true;
            sectionInfo.isAllAir = section->IsAllAir();
            sectionInfo.version++;        // 0->1 on first load, +1 on updates
            sectionInfo.state = SectionState::LOADED;
            if (!sectionInfo.isAllAir) {
                sectionInfo.dirty = true;
                chunk->AddDirty(y);
                m_chunksWithDirtySections.insert(chunkPos);
                m_schedulerSkip = 0;
            } else {
                sectionInfo.meshResolvedEmpty = true;
            }
        }
        {
            static bool s_loggedBiomes = false;
            if (!s_loggedBiomes && chunk->chunkData) {
                std::unordered_map<uint16_t, int> hist;
                for (int sy = 0; sy < Game::Math::SECTIONS_PER_CHUNK; ++sy) {
                    const auto* sec = chunk->chunkData->GetSection(sy);
                    if (!sec) continue;
                    sec->Biomes().ForEachValue([&](uint32_t id, int count) {
                        hist[static_cast<uint16_t>(id)] += count;
                    });
                }
                if (!hist.empty()) {
                    s_loggedBiomes = true;
                    std::string summary;
                    for (const auto& [id, n] : hist) {
                        summary += " " + std::string(Game::BiomeRegistry::Get(id).name)
                                 + "=" + std::to_string(n);
                    }
                    Log::Info("First chunk with biomes (%d, %d):%s",
                              chunkPos.x, chunkPos.z, summary.c_str());
                }
            }
        }
        
        // Transition to LOADED state
        TransitionChunkState(chunk, ChunkState::LOADED);
        
        // Mark neighbor chunks' sections as dirty for proper face culling
        MarkNeighborSectionsDirty(chunkPos);
        
        // Apply any pending diffs for this chunk
        ApplyPendingDiffsForChunk(chunkPos, chunk);

        // End-portal index for EndPortalRenderer. Rebuilt AFTER the diffs,
        // because those write straight into the chunk (they don't go through
        // SetBlockLocal) and would otherwise not be reflected. A partial
        // (non-groundUp) update rescans too: it can have replaced any section.
        RebuildEndPortalIndex(*chunk);

        // Mark all dirty sections for meshing
        for (int section : chunk->dirtySections) {
            // TODO: Queue mesh rebuild for this section
            Log::Debug("Section %d of chunk (%d, %d) marked for remeshing", 
                      section, chunkPos.x, chunkPos.z);
        }
    }
    
    void ClientChunkManager::RebuildEndPortalIndex(ClientChunk& chunk) {
        chunk.endPortals.clear();
        chunk.endGateways.clear();
        if (!chunk.chunkData) return;

        for (int sy = 0; sy < Game::Math::SECTIONS_PER_CHUNK; ++sy) {
            const Game::ChunkSection* section = chunk.chunkData->GetSection(sy);
            if (!section) continue;

            // Palette-membership pre-test. A section's palette is at most a
            // few dozen state ids, so this rejects every ordinary section for
            // the cost of a short linear scan instead of 4096 container reads
            // — which is what makes rebuilding on every chunk arrival free.
            // A section that has overflowed to the global palette has no
            // palette to test, so it falls through to the full walk.
            const Game::PalettedContainer& states = section->States();
            if (!states.IsGlobalPalette()) {
                bool present = false;
                for (uint32_t rawState : states.Palette()) {
                    const Game::BlockID id =
                        Game::BlockState::FromRawId(rawState).Block();
                    if (id == Game::BlockID::EndPortal ||
                        id == Game::BlockID::EndGateway) {
                        present = true;
                        break;
                    }
                }
                if (!present) continue;
            }

            const int baseY = Game::Math::WorldCoordinates::SectionCoordsToWorldY(sy, 0);
            for (int y = 0; y < Game::Math::SECTION_HEIGHT; ++y) {
                for (int z = 0; z < Game::Math::CHUNK_SIZE_Z; ++z) {
                    for (int x = 0; x < Game::Math::CHUNK_SIZE_X; ++x) {
                        const Game::BlockID id = section->GetBlockID(x, y, z);
                        if (id != Game::BlockID::EndPortal &&
                            id != Game::BlockID::EndGateway) {
                            continue;
                        }
                        const glm::ivec3 pos{
                            chunk.position.x * Game::Math::CHUNK_SIZE_X + x,
                            baseY + y,
                            chunk.position.z * Game::Math::CHUNK_SIZE_Z + z};
                        if (id == Game::BlockID::EndPortal) {
                            chunk.endPortals.push_back(pos);
                        } else {
                            chunk.endGateways.push_back(pos);
                        }
                    }
                }
            }
        }
    }

    void ClientChunkManager::ApplyPendingDiffsForChunk(Game::Math::ChunkPos chunkPos, ClientChunk* chunk) {
        if (!m_pendingDiffs || !chunk || !chunk->chunkData) {
            return;
        }
        
        // Get pending diffs for this chunk
        const auto* diffs = m_pendingDiffs->GetPendingDiffs(chunkPos);
        if (!diffs) {
            return;
        }
        
        Log::Debug("Applying pending diffs for chunk (%d, %d): %zu block changes", 
                  chunkPos.x, chunkPos.z, diffs->blockChanges.size());
        
        // Apply block changes
        for (const auto& [blockPos, change] : diffs->blockChanges) {
            // Check generation to avoid stale changes
            if (change.generation >= chunk->generation) {
                int localX = blockPos.x & 0xF;
                int localZ = blockPos.z & 0xF;
                int sectionY = (blockPos.y + 64) >> 4;
                
                chunk->chunkData->SetBlock(localX, blockPos.y, localZ, change.blockId, change.blockState);

                // This used to insert straight into `dirtySections` and stop
                // there, which is not enough on either count. The scheduler
                // skips a candidate whose `si.dirty` is false and whose
                // `version` has not moved past `meshingVersion`, so a diff
                // applied this way was written into the chunk and never
                // compiled; and, like every other write path, it has to
                // refresh the emptiness mirror or a diff that fills a
                // previously-air section is invisible. MarkSectionDirty does
                // both halves of the first, this does the second.
                RefreshSectionEmptiness(*chunk, sectionY);
                MarkSectionDirty(chunkPos, sectionY);

                Log::Debug("Applied pending block change at (%d, %d, %d) to block %d",
                         blockPos.x, blockPos.y, blockPos.z, static_cast<int>(change.blockId));
            }
        }
        
        // Apply light updates
        for (const auto& update : diffs->lightUpdates) {
            if (update.generation >= chunk->generation) {
                // TODO: Apply light update
                int sectionY = (update.pos.y + 64) >> 4;
                chunk->AddDirty(sectionY);
                m_chunksWithDirtySections.insert(chunkPos);
                m_schedulerSkip = 0;
            }
        }
        
        // Apply block entity updates
        for (const auto& [blockPos, update] : diffs->blockEntityUpdates) {
            if (update.generation >= chunk->generation) {
                // TODO: Apply block entity update
            }
        }
        
        // Remove the applied diffs
        size_t appliedCount = m_pendingDiffs->ApplyPendingDiffs(chunkPos, chunk->generation);
        if (appliedCount > 0) {
            Log::Info("Applied %zu pending diffs to chunk (%d, %d)", 
                     appliedCount, chunkPos.x, chunkPos.z);
        }
    }
    
    // Schedule mesh builds for dirty sections (Minecraft-style per-section)
    // Port of MC SectionRenderDispatcher.RenderSection.hasAllNeighbors:
    //
    //     doesChunkExistAt(WEST) && ... && doesChunkExistAt(1, 0, 1)
    //
    // — the four orthogonal and four diagonal chunk COLUMNS around this one,
    // each required to exist at FULL status. MC additionally requires
    // lightOnInColumn; we have no light engine, so "loaded" is the whole test.
    //
    // The section's own column is deliberately not checked: the caller already
    // holds it.
    bool ClientChunkManager::HasAllNeighborChunks(Game::Math::ChunkPos pos) const {
        static constexpr int kDX[8] = { -1, 0, 1, 0, -1, -1, 1, 1 };
        static constexpr int kDZ[8] = { 0, -1, 0, 1, -1, 1, -1, 1 };
        for (int i = 0; i < 8; ++i) {
            auto it = m_chunks.find(Game::Math::ChunkPos{pos.x + kDX[i], pos.z + kDZ[i]});
            if (it == m_chunks.end() || !it->second ||
                it->second->state != ChunkState::LOADED) {
                return false;
            }
        }
        return true;
    }

    void ClientChunkManager::ScheduleMeshBuildsWithSnapshots(const glm::vec3& playerPosition) {
        PROFILE_ZONE;
        ASSERT_MAIN_THREAD();

        // ADMISSION ONLY. This function decides what to hand to the compile
        // queue; it does NOT decide how fast meshing runs. That distinction is
        // the whole design and it is easy to undo by accident, so:
        //
        //   DO NOT gate this pass on the upload permit pool.
        //
        // MC's structure (LevelRenderer.compileSections:1129) is that every
        // dirty section is scheduled, every frame, with no cap of any kind —
        // `for (RenderSection s : sectionsToCompile) s.rebuildSectionAsync(cache)`.
        // The buffer pool is checked one level down, in
        // SectionRenderDispatcher.runTask:74, which gates EXECUTION. The queue
        // between them (CompileTaskDynamicQueue) is unbounded.
        //
        // We used to take a permit HERE, before a job was even queued, and cap
        // the pass at permits.Available(). That fused admission to execution and
        // made throughput `permits x passes/s`. Since this runs once per frame,
        // throughput became proportional to FRAME RATE — measured at 150-380 fps
        // on Vulkan versus 60-90 on OpenGL, which is why the two backends filled
        // visibly different amounts of world. Whenever throughput fell below
        // demand the backlog never drained, and because candidates are consumed
        // nearest-first the far corners of the square never got their turn: the
        // player saw a disc whose radius tracked frame rate.
        //
        // Demand, for reference:
        //     chunks/s (80.5) x sections/chunk (7.9) x remesh factor (2.2) ~ 1400/s
        //
        // The 2.2x remesh factor is NOT a bug to be optimised away: MC does the
        // same thing (ClientPacketListener.enableChunkLight ->
        // setSectionRangeDirty over the chunk AND its 8 neighbours, full Y range),
        // and gating compiles on neighbour availability would only make sections
        // appear LATER. Budget for it instead. MC's answer to it is the
        // recompile quota in CompileTaskDynamicQueue.poll, which we now have in
        // ClientWorkerPool::PollNearestLocked.
        //
        // No period gate either, matching MC. The dirty set now drains into the
        // compile queue every pass instead of accumulating behind a cap, so the
        // scan below is O(newly dirty) rather than O(backlog) — cheaper than the
        // visibleSections walk MC does unconditionally every frame.
        auto workerPool = Threading::g_clientWorkerPool.get();
        if (!workerPool) return;
        // Nothing dirty anywhere -> nothing to schedule. Every path that sets
        // SectionInfo::dirty also inserts into m_chunksWithDirtySections, so
        // this is exact, and it removes a per-frame walk of every visible
        // section (8k+ hash lookups, 1.3-2.9 ms/frame: 11% of the GL main
        // thread and 41% of the VK one in a fully loaded view, 2026-08-29).
        if (m_chunksWithDirtySections.empty()) return;
        // Idle backoff: a pass that found nothing to submit skips the next
        // three frames (the visible walk costs ~1.5 ms in a loaded view and
        // MC walks visibleSections every frame). Any dirty event resets it,
        // so block edits and chunk loads are never delayed; only a camera
        // turn onto already-dirty sections can wait up to 3 frames.
        if (m_schedulerSkip > 0) { --m_schedulerSkip; return; }
        // Eligibility pre-pass over the DIRTY set (small: hundreds), before
        // the visible walk (large: every section in view). A loaded view keeps
        // its outer ring dirty forever (no neighbours -> cannot mesh), so
        // "dirty set empty" never happens; "no dirty section is eligible" is
        // the steady state, and then the visible walk has nothing to find.
        // Eligibility changes only through MarkSectionDirty / chunk loads
        // (which re-dirty neighbours), so skipping is exact.
        {
            size_t eligible = 0;
            for (const auto& chunkPos : m_chunksWithDirtySections) {
                auto chunkIt = m_chunks.find(chunkPos);
                if (chunkIt == m_chunks.end() || !chunkIt->second->chunkData) continue;
                ClientChunk* chunk = chunkIt->second.get();
                for (uint32_t bits = chunk->dirtyMask; bits; bits &= bits - 1) {
                    const int sectionY = __builtin_ctz(bits);
                    if (sectionY >= Game::Math::SECTIONS_PER_CHUNK) break;
                    const auto& si = chunk->sectionInfos[sectionY];
                    if (!si.dirty || si.meshingVersion == si.version) continue;
                    if (!si.builtOnce && !NeighborsAllLoadedCached(*chunk)) continue;
                    ++eligible;
                    break;
                }
                if (eligible) break;
            }
            if (eligible == 0) return;
        }

        // Chunk-builder mode — MC's Options.prioritizeChunkUpdates, same three
        // values and the same default (NONE / fully threaded).
        //   0 Threaded       — everything async. MC's default, and ours.
        //   1 Semi Blocking  — PLAYER_AFFECTED: compile the player's own edits
        //                      on this thread so they appear the same frame.
        //   2 Fully Blocking — NEARBY: also compile anything within 768 (~27.7
        //                      blocks) synchronously.
        // Modes 1 and 2 spend main-thread milliseconds to remove a frame or two
        // of latency. That is the trade they exist to make; it is not free, and
        // it is why MC ships with them off.
        const int chunkBuilderMode = Platform::g_gameSettings.GetPrioritizeChunkUpdates();

        // Candidates come from the VISIBLE section list — MC
        // LevelRenderer.compileSections:1136, `for (RenderSection s : this.visibleSections)`.
        //
        // ── THE SEAM ──────────────────────────────────────────────────────────
        // This exact switch regressed meshing 954 -> 250 sections/s once before.
        // If a trace shows meshing collapsing again, flip kScheduleFromVisible
        // back to false — that single line restores the dirty-set behaviour and
        // nothing else depends on it.
        //
        // Three things had to be true first, and all three now are:
        //
        //  1. The visible list must contain UNMESHED sections. It previously
        //     held only sections that already had geometry, so scheduling from
        //     it would have queued re-meshes and never first meshes — the
        //     frontier could not advance at all. SectionOcclusionGraph now emits
        //     every reachable non-air section (MC runUpdates:253-257).
        //  2. The occlusion graph must keep pace. It updates incrementally every
        //     frame now (RunPartialUpdate), and propagation sources survive a
        //     rebuild instead of being discarded on anchor mismatch.
        //  3. The list must be the MAIN camera's. m_visibleSections is
        //     overwritten by the portal pass; GetMainViewSections() is the
        //     snapshot taken only for the real view.
        //
        // Gate to confirm from a trace: Occlusion/PartialAdded per second must
        // exceed the previous Upload/Sections x fps. If it does not, this switch
        // regresses by exactly that ratio.
        static constexpr bool kScheduleFromVisible = true;

        m_meshCandidates.clear();

        // ONE walk, over the dirty set. MC compileSections walks its
        // visibleSections and asks each "are you dirty?"; this used to do the
        // same — ~4,000 sections at a 32-chunk view, nearly all answering no,
        // 0.9 ms per run and the largest CPU item on the main thread after
        // the GPU waits (Tracy, 2026-09-04). The question is symmetric:
        // "needs a mesh" and "dirty" are the same flag, so walking the dirty
        // sections (tens, usually) and asking the renderer "are you in view?"
        // (one hash lookup against the main view's list, or a same-level
        // portal view's) collects the identical candidate set for a cost
        // proportional to what changed, not to what is on screen. The sort
        // below gives the same nearest-first priority either way.
        //
        // The dirty walk was always here as well, for what the visible list
        // cannot see: a section culled with a stale pre-explosion mask is not
        // in the list, so it never re-meshes, so its mask never updates, so
        // it stays culled — the "have to look around for it to render"
        // symptom. Dirty sections out of view are admitted only near the
        // player (3 chunks) or when the player's own edit dirtied them: a
        // block placed behind the player still compiles immediately (MC's
        // rebuildSectionSync), while the rest of the loaded world waits to
        // be looked at, MC-style.
        const bool haveView = kScheduleFromVisible && m_renderer;
        { PROFILE_ZONE_N("MeshSchedule.DirtyWalk");
        for (auto dirtyIt = m_chunksWithDirtySections.begin();
             dirtyIt != m_chunksWithDirtySections.end(); ) {
            const Game::Math::ChunkPos chunkPos = *dirtyIt;
            auto chunkIt = m_chunks.find(chunkPos);
            ClientChunk* chunk = (chunkIt != m_chunks.end()) ? chunkIt->second.get() : nullptr;

            if (!chunk || !chunk->chunkData || chunk->dirtySections.empty()) {
                // Unloaded or fully clean — drop from the index. Only advance via
                // erase's return iterator; nothing below mutates the set.
                if (!chunk || chunk->dirtySections.empty()) {
                    dirtyIt = m_chunksWithDirtySections.erase(dirtyIt);
                } else {
                    ++dirtyIt;  // Loaded but no CPU data yet — keep for later
                }
                continue;
            }
            ++dirtyIt;

            const float dx = chunkPos.x * 16.0f + 8.0f - playerPosition.x;
            const float dz = chunkPos.z * 16.0f + 8.0f - playerPosition.z;
            const float xzDistSq = dx * dx + dz * dz;

            // MC's hasAllNeighbors is a property of the COLUMN, but the
            // admission test below asks it per section. A freshly streamed
            // chunk arrives with all 24 sections dirty and none built, and
            // while its neighbours are still in flight every one of those
            // sections asked the same eight-hash-lookup question every frame
            // — 27% of the main thread's running time while flying, the
            // largest CPU item there (Instruments time profile, 2026-09-04).
            // Ask once per column per pass, and only if a section needs it.
            int neighborsLoaded = -1;   // -1 not asked yet, 0 no, 1 yes

            // The view test below is per section, but "no section of this
            // column is in any view" is one column lookup, and while flying
            // it is the answer for most dirty columns (everything streamed
            // in behind the player). Ask it once here; the per-section test
            // then runs only for columns some view actually reaches.
            const bool farColumn = haveView && xzDistSq > 48.0f * 48.0f;
            const bool columnInView = !farColumn ||
                                      m_renderer->IsMainViewColumn(chunkPos) ||
                                      m_renderer->IsPortalViewColumn(chunkPos);

            for (uint32_t bits = chunk->dirtyMask; bits; bits &= bits - 1) {
                const int sectionY = __builtin_ctz(bits);
                if (sectionY >= Game::Math::SECTIONS_PER_CHUNK) break;
                auto& si = chunk->sectionInfos[sectionY];
                if (!si.dirty) continue;
                if (si.meshingVersion == si.version) continue; // in flight

                // MC compileSections' admission test:
                //   isDirty() && (mesh != UNCOMPILED || hasAllNeighbors())
                // A section that has never been compiled waits until all eight
                // surrounding columns have arrived, so it is meshed once against
                // real neighbours instead of once against air and again after.
                // A section already compiled is always rescheduled.
                if (!si.builtOnce) {
                    if (neighborsLoaded < 0) neighborsLoaded = NeighborsAllLoadedCached(*chunk) ? 1 : 0;
                    if (neighborsLoaded == 0) continue;
                }

                if (farColumn && !si.dirtyFromPlayer &&
                    (!columnInView ||
                     (!m_renderer->IsMainViewSection(chunkPos, sectionY) &&
                      !m_renderer->IsPortalViewSection(chunkPos, sectionY)))) {
                    continue;
                }

                const float dy = (-64.0f + sectionY * 16.0f + 8.0f) - playerPosition.y;
                // Squared distance with Y attenuated (0.1 factor squared = 0.01)
                const float distSq = xzDistSq + dy * dy * 0.01f;

                // No initial-compile boost here any more. It used to multiply
                // initial compiles by 0.25 to jump them ahead of recompiles,
                // which only worked while this pass was also the throttle. The
                // job is now done properly one stage later by MC's recompile
                // quota (ClientWorkerPool::PollNearestLocked), which is a hard
                // guarantee rather than a distance fudge that a close enough
                // recompile could still beat. This ordering only decides who
                // gets a SNAPSHOT first when the budget below binds.
                m_meshCandidates.push_back({chunkPos, sectionY, distSq, chunk});
            }
        }

        PROFILE_PLOT("MeshSchedule/DirtyChunks", static_cast<int64_t>(m_chunksWithDirtySections.size()));
        }

        // Sort by squared distance (monotonic, same order as sqrt)
        std::sort(m_meshCandidates.begin(), m_meshCandidates.end(),
                  [](const auto& a, const auto& b) { return a.effectiveDistSq < b.effectiveDistSq; });

        // Submit snapshots.
        //
        // Two caps, and NEITHER is the upload permit pool:
        //
        // NO CAP OF ANY KIND. MC LevelRenderer.compileSections walks its whole
        // visibleSections list every frame and schedules every dirty one:
        //
        //     while (iter.hasNext()) { ... sectionsToCompile.add(section); }
        //     for (RenderSection s : sectionsToCompile) {
        //         s.rebuildSectionAsync(cache); s.setNotDirty();
        //     }
        //
        // Nothing counts, nothing breaks early. Both former caps are gone: the
        // compile queue is unbounded (as CompileTaskDynamicQueue is), and the
        // per-pass snapshot budget that used to sit here is gone too, now that
        // regions share their section copies through the cache below and
        // building one is 27 pointer copies plus whatever sections are new to
        // this pass.
        //
        // The ONLY throttle is the permit pool, one stage down in
        // ClientWorkerPool::WorkerLoop — MC's SectionRenderDispatcher.runTask
        // checking bufferPool before it starts a compile. Do not reintroduce a
        // cap here: capping admission rather than execution is what coupled
        // meshing throughput to frame rate and made a fast backend fill a
        // visibly larger disc of world than a slow one.
        // ONE cache for the whole pass, as MC constructs `new RenderRegionCache()`
        // at the top of compileSections. Every region built below shares its
        // section copies through it, so a cluster of neighbouring sections costs
        // roughly one copy per section instead of 27.
        Render::RenderRegionCache regionCache;

        size_t sectionsSubmitted = 0;
        static constexpr float kNearbySyncDistSq = 768.0f;  // MC LevelRenderer:1143

        { PROFILE_ZONE_N("MeshSchedule.Snapshots");
        for (const auto& candidate : m_meshCandidates) {
            auto* chunk = candidate.chunk;
            auto& sectionInfo = chunk->sectionInfos[candidate.sectionY];

            if (!sectionInfo.dirty || sectionInfo.meshingVersion == sectionInfo.version) continue;

            // Decide sync vs async BEFORE building the snapshot — neither input
            // needs it, and the async path may be out of queue room.
            //
            // MC LevelRenderer.compileSections:1140-1160. NEARBY uses a plain
            // (un-attenuated) squared distance, so recompute rather than reuse
            // candidate.effectiveDistSq, which de-weights Y for load ordering.
            bool rebuildSync = false;
            if (chunkBuilderMode == 2) {
                const float ddx = candidate.chunkPos.x * 16.0f + 8.0f - playerPosition.x;
                const float ddz = candidate.chunkPos.z * 16.0f + 8.0f - playerPosition.z;
                const float ddy = (-64.0f + candidate.sectionY * 16.0f + 8.0f) - playerPosition.y;
                const bool isNearby = (ddx * ddx + ddy * ddy + ddz * ddz) < kNearbySyncDistSq;
                rebuildSync = isNearby || sectionInfo.dirtyFromPlayer;
            } else if (chunkBuilderMode == 1) {
                rebuildSync = sectionInfo.dirtyFromPlayer;
            }

            const uint32_t expectedVersion = sectionInfo.version;

            std::shared_ptr<Render::MeshJobData> snapshot;
            if (!BuildSectionRegion(candidate.chunkPos, candidate.sectionY, expectedVersion,
                                    regionCache, snapshot)) {
                continue;
            }

            if (snapshot->region.CentreIsEmpty()) {
                snapshot->jobType = Render::MeshJobType::BorderOnly;
            } else if (!sectionInfo.builtOnce) {
                snapshot->jobType = Render::MeshJobType::Initial;
            } else {
                snapshot->jobType = Render::MeshJobType::Full;
            }

            snapshot->distanceToPlayer = candidate.effectiveDistSq;
            snapshot->isHighPriority = (candidate.effectiveDistSq < 16384.0f); // 128^2
            snapshot->submitTime = std::chrono::steady_clock::now();

            if (rebuildSync) {
                // Compiled on THIS thread and pushed straight to the upload
                // queue, which the MeshUpload phase drains later in this same
                // frame — so the edit is on screen without a round trip through
                // the worker pool. Bypasses the permit pool exactly like MC's
                // compileSync bypasses the buffer pool.
                if (sectionInfo.lastMeshJob) {
                    sectionInfo.lastMeshJob->Cancel();
                }
                sectionInfo.lastMeshJob = snapshot;
                sectionInfo.meshingVersion = expectedVersion;
                sectionInfo.state = SectionState::MESHING;
                sectionInfo.dirty = false;
                sectionInfo.dirtyFromPlayer = false;
                chunk->RemoveDirty(candidate.sectionY);
                if (chunk->dirtySections.empty()) {
                    m_chunksWithDirtySections.erase(candidate.chunkPos);
                }
                sectionsSubmitted++;
                workerPool->BuildMeshJobSync(snapshot);
                continue;
            }

            // No permit taken here. The job goes onto the compile queue and a
            // worker claims a pipeline slot when it actually starts the work —
            // ClientWorkerPool::WorkerLoop, mirroring MC's runTask()/bufferPool.
            std::shared_ptr<Render::MeshJobData> evictedJob;
            if (workerPool->SubmitMeshJobWithSnapshot(snapshot, &evictedJob)) {
                if (evictedJob) {
                    // Displaced by a nearer section: back to dirty so it is
                    // scheduled again once the queue has room.
                    NoteMeshBuildFailed(evictedJob->chunkPos, evictedJob->sectionY, evictedJob->generation);
                }
                if (sectionInfo.lastMeshJob) {
                    sectionInfo.lastMeshJob->Cancel();
                }
                sectionInfo.lastMeshJob = snapshot;
                sectionInfo.meshingVersion = expectedVersion;
                sectionInfo.state = SectionState::MESHING;
                sectionInfo.dirty = false;
                sectionInfo.dirtyFromPlayer = false;
                chunk->RemoveDirty(candidate.sectionY);
                if (chunk->dirtySections.empty()) {
                    m_chunksWithDirtySections.erase(candidate.chunkPos);
                }
                sectionsSubmitted++;
            } else {
                // Only reachable when the pool is shutting down — the queue no
                // longer refuses work. Nothing to release (no permit was taken)
                // and the section stays dirty, so nothing is lost either way.
                break;
            }
        }

        }
        m_schedulerSkip = m_meshCandidates.empty() ? 3u : 0u;
        if (sectionsSubmitted > 0) {
            // DistinctSections is the sharing ratio: with 27 sections per region
            // it should sit far below 27x the job count, and close to the job
            // count itself for a clustered visible set.
            Log::Debug("SCHEDULE: Submitted %zu mesh jobs (%zu candidates, "
                       "%zu distinct section copies this pass)",
                      sectionsSubmitted, m_meshCandidates.size(),
                      regionCache.DistinctSections());
        }
    }
    
    void ClientChunkManager::ScheduleDirtySectionMeshes() {
        // Get player position from somewhere (TODO: implement proper player tracking)
        glm::vec3 playerPos(0, 67, 0);
        ScheduleMeshBuildsWithSnapshots(playerPos);
    }
    
    bool ClientChunkManager::BuildSectionRegion(
        Game::Math::ChunkPos chunkPos, int sectionY,
        uint32_t expectedVersion,
        Render::RenderRegionCache& regionCache,
        std::shared_ptr<Render::MeshJobData>& outSnapshot) {

        PROFILE_ZONE_N("BuildRegion");
        ASSERT_MAIN_THREAD();

        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end() || !it->second || !it->second->chunkData) {
            return false;
        }

        auto& chunk = it->second;
        auto& sectionInfo = chunk->sectionInfos[sectionY];

        if (!sectionInfo.hasCpuData) return false;
        if (sectionInfo.version != expectedVersion) return false;

        outSnapshot = std::make_shared<Render::MeshJobData>(chunkPos, sectionY);
        outSnapshot->dimension = m_dimension;
        outSnapshot->generation = expectedVersion;
        outSnapshot->jobSeq = ++sectionInfo.lastJobSeq;

        // MC LevelRenderer.compileSections hands every section the SAME
        // RenderRegionCache, so the 3x3x3 neighbourhoods overlap and each
        // section is copied once per pass no matter how many jobs read it.
        outSnapshot->region = regionCache.CreateRegion(*this, chunkPos, sectionY);

        // Which horizontal neighbours were present, for FinalizeSectionUpload's
        // re-mesh decision. Derived from the region so it cannot disagree with
        // the data the mesher actually saw.
        uint8_t neighborMask = 0;
        if (outSnapshot->region.SectionForLocal(16, 0, 0))  neighborMask |= 1;  // +X
        if (outSnapshot->region.SectionForLocal(-1, 0, 0))  neighborMask |= 2;  // -X
        if (outSnapshot->region.SectionForLocal(0, 0, 16))  neighborMask |= 4;  // +Z
        if (outSnapshot->region.SectionForLocal(0, 0, -1))  neighborMask |= 8;  // -Z
        outSnapshot->neighborMask = neighborMask;

        // Final version check — the copies above are only valid for the version
        // they were taken at.
        if (sectionInfo.version != expectedVersion) {
            return false;
        }

        return true;
    }

    MeshAcceptance ClientChunkManager::AcceptMeshResult(const Network::MeshBuildResult& result) {
        PROFILE_ZONE_N("AcceptMeshResult");
        ASSERT_MAIN_THREAD();
        
        // Check if chunk still exists
        auto it = m_chunks.find(result.chunkPos);
        if (it == m_chunks.end() || !it->second) {
            // Chunk was unloaded while meshing
            // Log::Debug("[mesh] drop UNLOAD cx=%d cz=%d",
            //           result.chunkPos.x, result.chunkPos.z);
            return { MeshApplyAction::Drop_Unloaded };
        }
        
        auto& chunk = it->second;
        
        // Check if chunk is still loaded
        if (chunk->state != ChunkState::LOADED) {
            Log::Debug("Dropping mesh result - chunk (%d, %d) not in LOADED state",
                      result.chunkPos.x, result.chunkPos.z);
            return { MeshApplyAction::Drop_Unloaded };
        }
        
        // Check section bounds
        if (result.sectionY < 0 || result.sectionY >= 24) {
            Log::Warning("Dropping mesh result - invalid section %d", result.sectionY);
            return { MeshApplyAction::Drop_Unloaded };
        }
        
        auto& sectionInfo = chunk->sectionInfos[result.sectionY];

        // OUT-OF-ORDER GUARD: two jobs for the same section can complete on
        // different workers in either order. Uploading an older generation
        // OVER a newer one would leave the GPU holding blocks that no longer
        // exist while the version bookkeeping says it is current — and since
        // nothing would ever reschedule it (not dirty, meshingVersion ==
        // version), permanently.
        if (result.generation < sectionInfo.uploadedVersion) {
            return { MeshApplyAction::Drop_Replaced };
        }
        // Same-version tie-break: a re-dirty reschedules at an unchanged
        // version, so generations alone cannot order the two jobs — the seq
        // does. Dropping the older seq is what stops a stale-data result
        // (built before an ApplyChunkData resend or a mass-edit wave) from
        // overwriting the fresher mesh: the hole/ghost bug.
        if (result.jobSeq != 0 && result.jobSeq < sectionInfo.uploadedJobSeq) {
            return { MeshApplyAction::Drop_Replaced };
        }

        // Accept any result for a loaded chunk — better to show something than nothing.
        // FinalizeSectionUpload will mark it dirty for re-mesh if the version changed.
        
        // Version matches - good to upload!
        // Keep meshingVersion at the current version to prevent duplicate scheduling
        // It will be different from version if the section gets dirtied again
        
        Log::Debug("[mesh] ACCEPT: chunk(%d,%d) sy=%d ver=%u neighborMask=0x%X prevMask=0x%X",
                  result.chunkPos.x, result.chunkPos.z, result.sectionY, result.generation, 
                  result.neighborMask, sectionInfo.lastNeighborMask);
        
        return { MeshApplyAction::Upload };
    }
    
    void ClientChunkManager::FinalizeSectionUpload(Game::Math::ChunkPos chunkPos, int sectionY,
                                                   uint8_t neighborMask, uint32_t builtVersion,
                                                   uint32_t paletteGen,
                                                   uint32_t jobSeq) {
        ASSERT_MAIN_THREAD();
        
        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end() || !it->second) {
            // Chunk disappeared between accept and finalize (rare but possible)
            return;
        }
        
        auto& chunk = it->second;
        auto& sectionInfo = chunk->sectionInfos[sectionY];
        
        // Update neighbor mask BEFORE version check to prevent infinite loops
        // This ensures we don't keep rescheduling the same section thinking neighbors changed
        uint8_t prevMask = sectionInfo.lastNeighborMask;
        sectionInfo.lastNeighborMask = neighborMask;  // Update neighbor presence mask
        
        // Mark section as ready
        sectionInfo.state = SectionState::READY;
        sectionInfo.builtOnce = true;
        // The job only exists to be cancellable; once its result is applied it
        // is dead. Holding it kept every built section's RegionSnapshot (its 27
        // section copies) alive for the section's whole lifetime — measured
        // 2026-08-29: gigabytes at render distance 32, and freeing them all at
        // once on a far teleport was 174 ms of a 204 ms client stall.
        sectionInfo.lastMeshJob.reset();
        // The GPU now holds the mesh+mask for this content version. If the
        // section changed again while this mesh was in flight, version has
        // moved past builtVersion and the occlusion BFS keeps treating the
        // mask as stale (see SectionInfo::uploadedVersion).
        sectionInfo.uploadedVersion = builtVersion;
        sectionInfo.builtPaletteGen = paletteGen;
        if (jobSeq != 0) sectionInfo.uploadedJobSeq = jobSeq;

        // If version changed while meshing (neighbor loaded/unloaded), keep dirty for re-mesh
        // but still show this mesh result so the player sees something
        if (sectionInfo.version != sectionInfo.meshingVersion) {
            sectionInfo.meshingVersion = 0; // Allow rescheduling
            sectionInfo.dirty = true;
            chunk->AddDirty(sectionY);
            m_chunksWithDirtySections.insert(chunkPos);
            m_schedulerSkip = 0;
        } else {
            sectionInfo.dirty = false;
            chunk->RemoveDirty(sectionY);
        }
        
        // ── Convergence safety net ───────────────────────────────────────────
        // This mesh was built with a horizontal neighbour missing (treated as
        // air by the mesher) and that neighbour is loaded NOW. Whatever
        // ordering let that happen — the arrival's MarkNeighborSectionsDirty
        // raced this job, or an arrival path missed the dirtying — the result
        // on the GPU has border quads against a chunk that exists, and nothing
        // else is guaranteed to reschedule it. Re-dirty so the pipeline always
        // converges to the neighbour-complete mesh. Terminates: the rebuild
        // snapshots the neighbour, so its finalize carries the fuller mask and
        // this cannot re-fire for the same gap. All-air sections are skipped —
        // their mesh is empty regardless of neighbours.
        if ((neighborMask & 0xF) != 0xF) {
            ++m_partialNeighborBuilds;
            const uint8_t missingAtBuild = static_cast<uint8_t>(~neighborMask & 0xF);
            // Already-dirty sections are skipped: the branch above (version
            // moved while meshing — which is what a neighbour arrival's
            // MarkNeighborSectionsDirty does) has queued a rebuild against
            // fresh data, and double-marking would just bump the version again.
            if (!sectionInfo.dirty && !sectionInfo.isAllAir &&
                (missingAtBuild & CurrentNeighborMask(chunkPos)) != 0) {
                MarkSectionDirty(chunkPos, sectionY);
                ++m_staleBorderRemeshes;
            }
        }
        // ── Palette safety net ───────────────────────────────────────────────
        // Same convergence idea as the neighbour net above, for the greedy-
        // debug palette: a build that raced the toggle carries a stale stamp
        // and would otherwise keep the wrong colors until the next edit.
        if (paletteGen != ::Render::Mesher::GreedyPaletteGen() &&
            !sectionInfo.dirty && !sectionInfo.isAllAir) {
            MarkSectionDirty(chunkPos, sectionY);
        }

        // Cumulative count of uploads whose mesh saw a missing neighbour —
        // flat in a healthy run once loading settles; a run that lands in the
        // inflated-GPU state shows this climbing without matching remeshes.
        PROFILE_PLOT("Mesh/PartialNeighborBuilds",
                     static_cast<int64_t>(m_partialNeighborBuilds));

        Log::Debug("[mesh] FINALIZED: chunk(%d,%d) sy=%d neighborMask: 0x%X -> 0x%X (changed=%s)",
                  chunkPos.x, chunkPos.z, sectionY,
                  prevMask, neighborMask,
                  prevMask != neighborMask ? "YES" : "NO");
    }

    uint8_t ClientChunkManager::CurrentNeighborMask(Game::Math::ChunkPos pos) const {
        // Same bit layout as BuildSectionRegion derives from the region
        // snapshot: +X=1, -X=2, +Z=4, -Z=8.
        auto loaded = [this](int x, int z) {
            auto it = m_chunks.find(Game::Math::ChunkPos{x, z});
            return it != m_chunks.end() && it->second &&
                   it->second->state == ChunkState::LOADED;
        };
        uint8_t mask = 0;
        if (loaded(pos.x + 1, pos.z)) mask |= 1;
        if (loaded(pos.x - 1, pos.z)) mask |= 2;
        if (loaded(pos.x, pos.z + 1)) mask |= 4;
        if (loaded(pos.x, pos.z - 1)) mask |= 8;
        return mask;
    }
    
    void ClientChunkManager::NoteMeshBuildFailed(Game::Math::ChunkPos chunkPos, int sectionY,
                                                 uint32_t builtVersion) {
        ASSERT_MAIN_THREAD();
        if (sectionY < 0 || sectionY >= 24) return;
        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end() || !it->second) return;
        auto& chunk = it->second;
        auto& si = chunk->sectionInfos[sectionY];
        // Drop the cancellation handle only if it belongs to the job that just
        // failed. A STALE job's failure (an older generation, cancelled when a
        // newer job was submitted) used to clear the NEWER job's handle here,
        // which quietly disabled per-task cancellation for that section.
        if (si.lastMeshJob && si.lastMeshJob->generation == builtVersion) {
            si.lastMeshJob.reset();
        }
        // Only the LATEST job's failure needs recovery: a cancelled job always
        // has a newer one behind it (cancellation happens at the moment the
        // newer job is submitted), and that newer job carries the section
        // forward. But when the failed result IS the latest — its generation
        // matches what the scheduler recorded — nothing else will ever remesh
        // this section: dirty is false and meshingVersion == version reads as
        // "in flight" forever. Re-dirty it so the scheduler retries.
        if (si.meshingVersion == builtVersion && !si.dirty) {
            si.dirty = true;
            si.meshingVersion = 0;
            chunk->AddDirty(sectionY);
            m_chunksWithDirtySections.insert(chunkPos);
            m_schedulerSkip = 0;
        }
    }

    void ClientChunkManager::DiscardRetained(Game::Math::ChunkPos pos) {
        auto it = m_retained.find(pos);
        if (it == m_retained.end()) return;
        if (m_meshes) m_meshes->DiscardParkedChunkGPUData(pos);
        m_retainedBytes -= std::min(m_retainedBytes, it->second.bytes);
        m_retainedLru.erase(it->second.lru);
        m_retained.erase(it);
    }

    bool ClientChunkManager::RestoreRetainedChunk(Game::Math::ChunkPos pos, uint64_t modStamp) {
        ASSERT_MAIN_THREAD();
        auto it = m_retained.find(pos);
        if (it == m_retained.end()) return false;
        if (!it->second.chunk || !it->second.chunk->chunkData ||
            it->second.chunk->chunkData->ModStamp() != modStamp) {
            DiscardRetained(pos);
            return false;
        }
        if (m_chunks.count(pos)) {   // a full load raced ahead; keep it
            DiscardRetained(pos);
            return true;
        }
        std::unique_ptr<ClientChunk> chunk = std::move(it->second.chunk);
        m_retainedBytes -= std::min(m_retainedBytes, it->second.bytes);
        m_retainedLru.erase(it->second.lru);
        m_retained.erase(it);
        ClientChunk* raw = chunk.get();
        raw->generation = m_nextGeneration.fetch_add(1);
        for (int sy = 0; sy < Game::Math::SECTIONS_PER_CHUNK; ++sy) {
            auto& si = raw->sectionInfos[sy];
            si.lastMeshJob.reset();
            si.dirtyFromPlayer = false;
        }
        m_chunks.emplace(pos, std::move(chunk));
        TransitionChunkState(raw, ChunkState::LOADED);
        if (m_meshes) m_meshes->UnparkChunkGPUData(pos);
        if (!raw->dirtySections.empty()) { m_chunksWithDirtySections.insert(pos); m_schedulerSkip = 0; }

        // ── Reconcile neighbour-presence with the meshes on both sides ──────
        //
        // This restore is the ONE chunk-arrival path that used not to dirty
        // anything, and it is how the ~2x GPU-section state was born: unload
        // dirties the 4 neighbours (UnloadChunk), they remesh against the now
        // MISSING chunk — the mesher reads a missing section as air
        // (RegionSnapshot::SectionForLocal -> nullptr), so solid underground
        // sections grow a full wall of border quads — and then the chunk came
        // back through this 20-byte ChunkUnchangedS2C path with no dirtying at
        // all. Nothing ever rescheduled those sections (not dirty, versions
        // match), so the air-built border geometry persisted for the session.
        // MC has no retention cache; its equivalent arrival path
        // (ClientChunkCache.replace -> onChunkLoaded) always dirties the ring.
        //
        // Targeted, not MarkNeighborSectionsDirty: blanket-dirtying 4x24
        // sections on every revisit would defeat the cache's "no meshing on
        // revisit" purpose. lastNeighborMask records which neighbours each
        // mesh was actually built against, so only sections meshed while a
        // now-present chunk was absent (or vice versa) are re-dirtied — on a
        // clean park/revive round-trip that is zero sections.
        {
            struct { int dx, dz; uint8_t bitTowardUs; } const kDirs[4] = {
                { +1, 0, 2 },   // east neighbour sees us as its -X
                { -1, 0, 1 },   // west neighbour sees us as its +X
                { 0, +1, 8 },   // south neighbour sees us as its -Z
                { 0, -1, 4 },   // north neighbour sees us as its +Z
            };
            for (const auto& d : kDirs) {
                const Game::Math::ChunkPos nPos{pos.x + d.dx, pos.z + d.dz};
                auto nIt = m_chunks.find(nPos);
                if (nIt == m_chunks.end() || nIt->second->state != ChunkState::LOADED) continue;
                for (int sy = 0; sy < Game::Math::SECTIONS_PER_CHUNK; ++sy) {
                    const auto& nsi = nIt->second->sectionInfos[sy];
                    // Meshed while we were absent (mask bit clear) -> its
                    // border wall must come down now that we exist again.
                    // All-air sections have no geometry either way; already
                    // dirty ones will be rebuilt against fresh data anyway.
                    if (!nsi.builtOnce || nsi.isAllAir || nsi.dirty) continue;
                    if ((nsi.lastNeighborMask & d.bitTowardUs) != 0) continue;
                    MarkSectionDirty(nPos, sy);
                    ++m_staleBorderRemeshes;
                }
            }

            // And our own revived meshes: if the set of loaded horizontal
            // neighbours differs from what they were built against — a
            // neighbour unloaded or arrived while we were parked, which
            // MarkNeighborSectionsDirty could not tell us about before it
            // learned to reach parked chunks — rebuild those sections too.
            const uint8_t presentNow = CurrentNeighborMask(pos);
            for (int sy = 0; sy < Game::Math::SECTIONS_PER_CHUNK; ++sy) {
                const auto& si = raw->sectionInfos[sy];
                if (!si.builtOnce || si.isAllAir || si.dirty) continue;
                // A stale greedy-debug palette also forces a rebuild: the
                // toggle's RemeshAll cannot reach a chunk that is parked, so
                // revival is where its meshes catch up with the palette.
                const bool paletteStale =
                    si.builtPaletteGen != ::Render::Mesher::GreedyPaletteGen();
                if (!paletteStale && si.lastNeighborMask == presentNow) continue;
                MarkSectionDirty(pos, sy);
                if (!paletteStale) ++m_staleBorderRemeshes;
            }
        }

        // A neighbour that comes back CHANGED while we are LOADED re-dirties
        // our border sections through its own ApplyChunkData ->
        // MarkNeighborSectionsDirty (which now also reaches us while parked).
        ApplyPendingDiffsForChunk(pos, raw);
        ++m_retainRestored;
        return true;
    }

    void ClientChunkManager::ClearAllChunks() {
        ASSERT_MAIN_THREAD();
        // Cancel every in-flight job first: a result landing after the
        // chunks are gone would be dropped anyway, and cancelling stops the
        // worker spending a permit on it. Live GPU data goes with the chunks
        // — only parked data was released here before, which leaked a whole
        // level's meshes on every dimension change.
        for (auto& [pos, chunk] : m_chunks) {
            if (!chunk) continue;
            for (auto& si : chunk->sectionInfos) {
                if (si.lastMeshJob) { si.lastMeshJob->Cancel(); si.lastMeshJob.reset(); }
            }
        }
        while (!m_retainedLru.empty()) DiscardRetained(m_retainedLru.back());
        if (m_meshes) {
            for (const auto& [pos, chunk] : m_chunks) m_meshes->RemoveChunkGPUData(pos);
        }
        m_chunks.clear();
        m_loadedChunkCount = 0;
        m_chunksWithDirtySections.clear();
        // Predictions reference positions in chunks that no longer exist —
        // drop them rather than letting a late ack roll back into a chunk
        // that has since been reloaded from scratch.
        m_prediction.Clear();
#if ENABLE_IMMERSIVE_PORTALS
        GetClientImmersivePortals().Clear();
#endif
        Log::Info("Cleared all chunks from ClientChunkManager");
    }
    
    // ========================================================================
    // RENDER GRID SYNCHRONIZATION
    // ========================================================================
    
    void ClientChunkManager::SnapshotLoadedChunks(
            std::vector<std::pair<Game::Math::ChunkPos, ClientChunk*>>& out) const {
        ASSERT_MAIN_THREAD();
        out.clear();
        out.reserve(m_chunks.size());
        
        for (const auto& [pos, chunk] : m_chunks) {
            if (chunk && chunk->IsLoaded()) {
                out.emplace_back(pos, chunk.get());
            }
        }
    }
    
    void ClientChunkManager::NotifyRenderGridChunkLoaded(Game::Math::ChunkPos pos, ClientChunk* chunk) {
        ASSERT_MAIN_THREAD();
        // Notify the occlusion graph that a chunk loaded — sections deferred by
        // hasAllNeighbors will be re-evaluated on the next BFS rebuild.
        if (m_renderer) {
            m_renderer->MarkVisibleSectionsDirty();
        }
    }
    
    void ClientChunkManager::NotifyRenderGridChunkUnloaded(Game::Math::ChunkPos pos) {
        ASSERT_MAIN_THREAD();
        if (m_renderer) {
            m_renderer->MarkVisibleSectionsDirty();
        }
    }
    
    void ClientChunkManager::NotifyRenderGridSectionUpdated(Game::Math::ChunkPos pos, int sectionY, 
                                                           ::Render::GPUSectionData* gpu) {
        ASSERT_MAIN_THREAD();
        // No-op: RenderGrid has been removed
    }

} // namespace Client