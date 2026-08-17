// File: src/client/world/ClientChunkManager.cpp
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
#include "platform/GameDirectory.hpp"
#include <glad/glad.h>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>

namespace Client {

    // Global instance
    std::unique_ptr<ClientChunkManager> g_clientChunkManager = nullptr;


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
            [this](const glm::ivec3& pos, Game::BlockID state, uint8_t stateIndex) {
                SetBlockLocal(pos, state, stateIndex);
            },
            [this](const glm::ivec3& pos) { return GetBlockAndStateAt(pos); });

        Log::Info("ClientChunkManager initialized successfully");
    }

    void ClientChunkManager::Shutdown() {
        ASSERT_MAIN_THREAD();
        Log::Info("Shutting down ClientChunkManager...");
        
        // Clear all chunks
        m_chunks.clear();
        
        // Clear pending diffs
        if (m_pendingDiffs) {
            m_pendingDiffs->ClearAll();
        }
        
        Log::Info("ClientChunkManager shutdown complete");
    }

    // ========================================================================
    // CHUNK STATE MANAGEMENT
    // ========================================================================

    
    void ClientChunkManager::LoadChunk(Game::Math::ChunkPos chunkPos, const Network::SerializedChunkData& serializedData) {
        PROFILE_ZONE;
        ASSERT_MAIN_THREAD();
        Log::Debug("PIPELINE: LoadChunk START for chunk (%d, %d) with %zu bytes", 
                  chunkPos.x, chunkPos.z, serializedData.GetTotalSize());
        
        // Get or create client chunk
        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end()) {
            // Create new client chunk
            Log::Debug("PIPELINE: Creating new ClientChunk for (%d, %d)", chunkPos.x, chunkPos.z);
            auto clientChunk = std::make_unique<ClientChunk>(chunkPos);
            it = m_chunks.emplace(chunkPos, std::move(clientChunk)).first;
        } else {
            Log::Debug("PIPELINE: Using existing ClientChunk for (%d, %d)", chunkPos.x, chunkPos.z);
        }
        
        ClientChunk* chunk = it->second.get();
        
        // Deserialize chunk data
        Log::Debug("PIPELINE: Deserializing chunk data for (%d, %d)", chunkPos.x, chunkPos.z);
        chunk->chunkData = DeserializeChunkData(serializedData);
        if (!chunk->chunkData) {
            Log::Error("PIPELINE: Failed to deserialize chunk data for (%d, %d)", chunkPos.x, chunkPos.z);
            return;
        }
        
        Log::Debug("PIPELINE: Chunk data deserialized successfully for (%d, %d), %zu sections created",
                  chunkPos.x, chunkPos.z, chunk->chunkData->GetSectionCount());

        // Initialize ALL 24 sections for proper neighbor culling (Minecraft-style)
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            auto& sectionInfo = chunk->sectionInfos[sectionY];
            sectionInfo.lastMeshJob.reset(); // Clear any old task reference

            if (chunk->chunkData->HasSection(sectionY)) {
                // Section has data — reset all state for fresh meshing
                sectionInfo.hasCpuData = true;
                sectionInfo.isAllAir = false;
                sectionInfo.state = SectionState::LOADED;
                sectionInfo.version++;
                sectionInfo.meshingVersion = 0;
                sectionInfo.builtOnce = false;
                sectionInfo.dirty = true;
                
                // Also update legacy dirty set for compatibility
                chunk->dirtySections.insert(sectionY);
                m_chunksWithDirtySections.insert(chunkPos);
            } else {
                // Section is empty (all air) — reset state
                sectionInfo.hasCpuData = true;
                sectionInfo.isAllAir = true;
                sectionInfo.state = SectionState::LOADED;
                sectionInfo.version++;
                sectionInfo.meshingVersion = 0;
                sectionInfo.builtOnce = false;
                sectionInfo.dirty = false;
            }
        }
        
        // Transition to LOADED state
        TransitionChunkState(chunk, ChunkState::LOADED);
        
        // Mark neighbor chunks' sections as dirty for proper face culling
        MarkNeighborSectionsDirty(chunkPos);
        
        // Count how many sections have data vs are empty
        int nonEmptyCount = 0;
        for (int sy = 0; sy < Game::Math::SECTIONS_PER_CHUNK; ++sy) {
            if (!chunk->sectionInfos[sy].isAllAir) {
                nonEmptyCount++;
            }
        }
        
        Log::Debug("PIPELINE: LoadChunk COMPLETE for chunk (%d, %d) - %d non-empty sections, %zu dirty sections", 
                  chunkPos.x, chunkPos.z, nonEmptyCount, chunk->dirtySections.size());
    }


    void ClientChunkManager::UnloadChunk(Game::Math::ChunkPos chunkPos) {
        PROFILE_ZONE;
        ASSERT_MAIN_THREAD();
        Log::Debug("CLIENT UNLOAD: chunk (%d, %d)", chunkPos.x, chunkPos.z);

        // Mark neighbor chunks' sections as dirty BEFORE unloading
        // This ensures they rebuild their meshes to show previously culled faces
        MarkNeighborSectionsDirty(chunkPos);
        
        // Cancel in-flight mesh tasks per-section (Minecraft-style per-task cancellation)
        {
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
            m_pendingDiffs->DropChunkDiffs(chunkPos);
        }

        // Clean up GPU resources (vertex/index buffers) before erasing chunk data
        if (::Render::g_clientMeshManager) {
            ::Render::g_clientMeshManager->RemoveChunkGPUData(chunkPos);
        }

        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end()) {
            m_chunks.erase(it);
            Log::Debug("Unloaded chunk (%d, %d)", chunkPos.x, chunkPos.z);
        }

        // The scheduler no longer walks this index (candidates come from the
        // visible list), so it has to be pruned here — a stale entry would keep
        // ScheduleMeshBuildsWithSnapshots' empty() early-out from ever firing.
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
            it->second->dirtySections.insert(sectionY);  // Keep as index for iteration
            m_chunksWithDirtySections.insert(chunkPos);
            Log::Debug("Marked chunk (%d, %d) section %d as dirty (version now %u)",
                      chunkPos.x, chunkPos.z, sectionY, sectionInfo.version);
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
                it->second->dirtySections.insert(sectionY);  // Keep as index for iteration
            }
            m_chunksWithDirtySections.insert(chunkPos);
            Log::Debug("Marked all sections in chunk (%d, %d) as dirty", chunkPos.x, chunkPos.z);
        }
    }
    
    void ClientChunkManager::ClearSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY) {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end()) {
            size_t removed = it->second->dirtySections.erase(sectionY);
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
                neighborIt->second->dirtySections.insert(sectionY);
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
            it->second->UpdateAccessTime();
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
        size_t count = 0;
        for (const auto& [pos, chunk] : m_chunks) {
            if (chunk && chunk->state == ChunkState::LOADED) {
                count++;
            }
        }
        return count;
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

    std::shared_ptr<Game::Chunk> ClientChunkManager::DeserializeChunkData(const Network::SerializedChunkData& serializedData) {
        auto chunk = std::make_shared<Game::Chunk>();
        
        // Deserialize chunk data from the packet
        if (serializedData.blockData.empty()) {
            Log::Debug("Empty block data in serialized chunk, creating empty chunk");
            return chunk;
        }
        
        Log::Debug("Deserializing chunk data: {} bytes, compression type: {}",
                  serializedData.blockData.size(), serializedData.compressionType);
        
        // Handle different compression types
        const std::vector<uint8_t>* blockData = &serializedData.blockData;
        std::vector<uint8_t> decompressedData;
        
        if (serializedData.compressionType != 0) {
            // TODO: Implement decompression for other compression types
            Log::Warning("Compression type {} not implemented, treating as uncompressed",
                       serializedData.compressionType);
        }
        
        // Deserialize sections from block data
        const uint8_t* dataPtr = blockData->data();
        size_t dataOffset = 0;
        size_t totalDataSize = blockData->size();
        
        Log::Debug("Starting deserialization: {} total bytes", totalDataSize);
        
        // Process each section
        for (int sectionIndex = 0; sectionIndex < Game::Math::SECTIONS_PER_CHUNK; ++sectionIndex) {
            // Check if we have enough data for a full section
            size_t sectionSize = 16 * 16 * 16 * sizeof(uint16_t); // 8192 bytes per section
            
            if (dataOffset + sectionSize > totalDataSize) {
                // Not enough data for this section, it's likely empty
                Log::Debug("Section {} has no data (offset {} + {} > {})",
                         sectionIndex, dataOffset, sectionSize, totalDataSize);
                break;
            }
            
            // Check if this section contains any non-air blocks
            const uint16_t* sectionBlocks = reinterpret_cast<const uint16_t*>(dataPtr + dataOffset);
            bool hasBlocks = false;
            
            // Quick scan to see if section has any blocks
            for (size_t i = 0; i < 16 * 16 * 16; ++i) {
                if (sectionBlocks[i] != static_cast<uint16_t>(Game::BlockID::Air)) {
                    hasBlocks = true;
                    break;
                }
            }
            
            if (hasBlocks) {
                // Create and populate the section
                chunk->EnsureSection(sectionIndex);
                auto* section = chunk->GetSection(sectionIndex);
                
                if (section) {
                    // Per voxel: the section's storage is a paletted container
                    // now, so there is no flat array to memcpy into.
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(sectionBlocks);
                    for (int y = 0; y < Game::ChunkSection::SIZE; ++y) {
                        for (int z = 0; z < Game::ChunkSection::SIZE; ++z) {
                            for (int x = 0; x < Game::ChunkSection::SIZE; ++x) {
                                section->Set(x, y, z, src[y * 256 + z * 16 + x]);
                            }
                        }
                    }
                    (void)sectionSize;

                    Log::Debug("Deserialized section {} with block data", sectionIndex);
                } else {
                    Log::Debug("Failed to create section {} during deserialization", sectionIndex);
                }
            }
            
            dataOffset += sectionSize;
        }
        
        Log::Debug("Chunk deserialization completed: {} sections processed, {} bytes consumed",
                  Game::Math::SECTIONS_PER_CHUNK, dataOffset);
        
        return chunk;
    }

    void ClientChunkManager::TransitionChunkState(ClientChunk* chunk, ChunkState newState) {
        if (!chunk) {
            return;
        }
        
        ChunkState oldState = chunk->state;
        chunk->state = newState;
        
        // Notify RenderGrid when chunk becomes loaded
        if (newState == ChunkState::LOADED && oldState != ChunkState::LOADED) {
            NotifyRenderGridChunkLoaded(chunk->position, chunk);
        }
        // Notify RenderGrid when chunk becomes unloaded
        else if (oldState == ChunkState::LOADED && newState != ChunkState::LOADED) {
            NotifyRenderGridChunkUnloaded(chunk->position);
        }
    }


    // ========================================================================
    // GLOBAL FUNCTIONS
    // ========================================================================

    void InitializeClientChunkManager() {
        if (g_clientChunkManager) {
            Log::Warning("ClientChunkManager already initialized");
            return;
        }

        g_clientChunkManager = std::make_unique<ClientChunkManager>();
        g_clientChunkManager->Initialize();
    }

    void ShutdownClientChunkManager() {
        if (g_clientChunkManager) {
            g_clientChunkManager->Shutdown();
            g_clientChunkManager.reset();
        }
    }

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
                                           uint8_t stateIndex, bool fromPlayer) {
        ASSERT_MAIN_THREAD();
        Game::Math::ChunkPos chunkPos{pos.x >> 4, pos.z >> 4};

        ClientChunk* chunk = GetChunk(chunkPos);
        if (!chunk || !chunk->chunkData) return;

        // Calculate local block position within chunk
        int localX = pos.x & 0xF;  // mod 16
        int localZ = pos.z & 0xF;
        int sectionY = (pos.y + 64) >> 4;  // Convert to section index

        // Set the block in the chunk
        chunk->chunkData->SetBlock(localX, pos.y, localZ, blockId, stateIndex);

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

    std::pair<Game::BlockID, uint8_t> ClientChunkManager::GetBlockAndStateAt(const glm::ivec3& pos) const {
        const ClientChunk* chunk = GetChunk({pos.x >> 4, pos.z >> 4});
        if (!chunk || !chunk->chunkData) return {Game::BlockID::Air, 0};
        const int lx = pos.x & 0xF, lz = pos.z & 0xF;
        return { chunk->chunkData->GetBlock(lx, pos.y, lz),
                 chunk->chunkData->GetBlockState(lx, pos.y, lz) };
    }

    void ClientChunkManager::PredictBlockChange(const glm::ivec3& pos,
                                                Game::BlockID newBlock,
                                                uint32_t sequence,
                                                uint8_t stateIndex) {
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
        if (!chunk->chunkData || packet.groundUpContinuous) {
            chunk->chunkData = std::make_shared<Game::Chunk>();
        }
        
        // Initialize ALL sections first (for ground-up loads)
        if (packet.groundUpContinuous) {
            for (int y = 0; y < Game::Math::SECTIONS_PER_CHUNK; ++y) {
                auto& sectionInfo = chunk->sectionInfos[y];
                // Reset to default state for ground-up load
                sectionInfo.hasCpuData = true;  // We'll know the state after parsing
                sectionInfo.isAllAir = true;    // Assume air until proven otherwise
                sectionInfo.state = SectionState::LOADED;
                sectionInfo.version = 0;
                sectionInfo.dirty = false;
                sectionInfo.meshingVersion = 0;
            }
        }
        
        // Apply section data
        int sectionIndex = 0;
        for (int y = 0; y < Game::Math::SECTIONS_PER_CHUNK; ++y) {
            if (packet.primaryBitmask & (1 << y)) {
                if (sectionIndex < packet.sections.size()) {
                    const auto& sectionData = packet.sections[sectionIndex];
                    
                    // Ensure section exists
                    chunk->chunkData->EnsureSection(y);
                    auto* section = chunk->chunkData->GetSection(y);
                    
                    if (section && !sectionData.IsEmpty()) {
                        // MC LevelChunkSection.read: hand the wire's bits,
                        // palette and words straight to the container. No
                        // per-voxel unpacking — the words ARE the storage.
                        Game::PalettedContainer states(
                            Game::PaletteStrategy::ForBlockStates(Game::BlockStateIds::Bits()),
                            Game::BlockStateIds::Pack(Game::BlockID::Air, 0));
                        Game::PalettedContainer biomes = Game::ChunkSection::MakeBiomeContainer();

                        auto stateWords  = sectionData.states.words;
                        auto statePal    = sectionData.states.palette;
                        auto biomeWords  = sectionData.biomes.words;
                        auto biomePal    = sectionData.biomes.palette;

                        const bool okStates = states.ReadFrom(
                            sectionData.states.bits, std::move(statePal), std::move(stateWords));
                        const bool okBiomes = biomes.ReadFrom(
                            sectionData.biomes.bits, std::move(biomePal), std::move(biomeWords));

                        if (okStates && okBiomes) {
                            section->AdoptStates(std::move(states));
                            section->AdoptBiomes(std::move(biomes));

                            auto& sectionInfo = chunk->sectionInfos[y];
                            sectionInfo.hasCpuData = true;
                            sectionInfo.isAllAir = section->IsAllAir();
                            sectionInfo.version++;        // 0->1 on first load, +1 on updates
                            sectionInfo.state = SectionState::LOADED;
                            sectionInfo.dirty = true;     // Needs meshing

                            chunk->dirtySections.insert(y);
                            m_chunksWithDirtySections.insert(chunkPos);
                        } else {
                            Log::Warning("Failed to decode section %d for chunk (%d, %d)",
                                       y, chunkPos.x, chunkPos.z);
                        }
                    }
                    
                    sectionIndex++;
                }
            } else if (packet.groundUpContinuous) {
                // For ground-up loads, sections not in bitmask are empty
                auto& sectionInfo = chunk->sectionInfos[y];
                sectionInfo.version++;  // Still increment version for neighbor culling
                // hasCpuData and isAllAir already set to true in initialization
            }
        }
        
        // Biomes arrived inside each section's container above (MC keeps them
        // on LevelChunkSection, not the chunk). This one-shot log stays because
        // a silent break anywhere upstream just looks like "every chunk is
        // plains", which is indistinguishable from a colour bug on screen.
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
        
        // Mark all dirty sections for meshing
        for (int section : chunk->dirtySections) {
            // TODO: Queue mesh rebuild for this section
            Log::Debug("Section %d of chunk (%d, %d) marked for remeshing", 
                      section, chunkPos.x, chunkPos.z);
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
                chunk->dirtySections.insert(sectionY);
                m_chunksWithDirtySections.insert(chunkPos);

                Log::Debug("Applied pending block change at (%d, %d, %d) to block %d",
                         blockPos.x, blockPos.y, blockPos.z, static_cast<int>(change.blockId));
            }
        }
        
        // Apply light updates
        for (const auto& update : diffs->lightUpdates) {
            if (update.generation >= chunk->generation) {
                // TODO: Apply light update
                int sectionY = (update.pos.y + 64) >> 4;
                chunk->dirtySections.insert(sectionY);
                m_chunksWithDirtySections.insert(chunkPos);
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
        bool usedVisibleList = false;

        if (kScheduleFromVisible && ::Render::g_chunkRenderer) {
            const auto& visible = ::Render::g_chunkRenderer->GetMainViewSections();
            for (const auto& vs : visible) {
                auto vIt = m_chunks.find(vs.chunkPos);
                if (vIt == m_chunks.end()) continue;
                ClientChunk* chunk = vIt->second.get();
                if (!chunk || !chunk->chunkData) continue;
                if (vs.sectionY < 0 || vs.sectionY >= Game::Math::SECTIONS_PER_CHUNK) continue;

                auto& si = chunk->sectionInfos[vs.sectionY];
                if (!si.dirty) continue;
                if (si.meshingVersion == si.version) continue;  // in flight

                // MC compileSections' admission test:
                //   isDirty() && (mesh != UNCOMPILED || hasAllNeighbors())
                // A section that has never been compiled waits until all eight
                // surrounding columns have arrived, so it is meshed once against
                // real neighbours instead of once against air and again after.
                // A section already compiled is always rescheduled.
                if (!si.builtOnce && !HasAllNeighborChunks(vs.chunkPos)) continue;

                const float dx = vs.chunkPos.x * 16.0f + 8.0f - playerPosition.x;
                const float dz = vs.chunkPos.z * 16.0f + 8.0f - playerPosition.z;
                const float dy = (-64.0f + vs.sectionY * 16.0f + 8.0f) - playerPosition.y;
                m_meshCandidates.push_back(
                    {vs.chunkPos, vs.sectionY, dx * dx + dz * dz + dy * dy * 0.01f, chunk});
            }

            usedVisibleList = true;
        }

        // The dirty walk. It is the whole candidate source when scheduling from
        // the visible list is off, and in modes 1/2 it additionally runs
        // alongside it to pick up the player's OWN edits.
        //
        // Sync compiles must not be gated on visibility: a block placed behind
        // the player still has to compile immediately, which is the entire point
        // of MC's rebuildSectionSync. Anything already collected above is
        // filtered out by the dirty/meshingVersion checks in the submit loop, so
        // the overlap costs nothing but a skipped iteration.
        const bool needDirtyWalk = !usedVisibleList || chunkBuilderMode != 0;
        for (auto dirtyIt = m_chunksWithDirtySections.begin();
             needDirtyWalk && dirtyIt != m_chunksWithDirtySections.end(); ) {
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

            for (int sectionY : chunk->dirtySections) {
                if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) continue;
                auto& si = chunk->sectionInfos[sectionY];
                if (!si.dirty) continue;
                if (si.meshingVersion == si.version) continue; // in flight

                // When the visible list already supplied candidates, this pass
                // exists only for the player's own edits — everything else is
                // visibility-gated, MC-style.
                if (usedVisibleList && !si.dirtyFromPlayer) continue;

                // Same admission test as above (MC applies it in one loop).
                if (!si.builtOnce && !HasAllNeighborChunks(chunkPos)) continue;

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
                chunk->dirtySections.erase(candidate.sectionY);
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
            if (workerPool->SubmitMeshJobWithSnapshot(snapshot)) {
                if (sectionInfo.lastMeshJob) {
                    sectionInfo.lastMeshJob->Cancel();
                }
                sectionInfo.lastMeshJob = snapshot;
                sectionInfo.meshingVersion = expectedVersion;
                sectionInfo.state = SectionState::MESHING;
                sectionInfo.dirty = false;
                sectionInfo.dirtyFromPlayer = false;
                chunk->dirtySections.erase(candidate.sectionY);
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
        outSnapshot->generation = expectedVersion;

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
    
    void ClientChunkManager::FinalizeSectionUpload(Game::Math::ChunkPos chunkPos, int sectionY, uint8_t neighborMask) {
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

        // If version changed while meshing (neighbor loaded/unloaded), keep dirty for re-mesh
        // but still show this mesh result so the player sees something
        if (sectionInfo.version != sectionInfo.meshingVersion) {
            sectionInfo.meshingVersion = 0; // Allow rescheduling
            sectionInfo.dirty = true;
            chunk->dirtySections.insert(sectionY);
            m_chunksWithDirtySections.insert(chunkPos);
        } else {
            sectionInfo.dirty = false;
            chunk->dirtySections.erase(sectionY);
        }
        
        Log::Debug("[mesh] FINALIZED: chunk(%d,%d) sy=%d neighborMask: 0x%X -> 0x%X (changed=%s)",
                  chunkPos.x, chunkPos.z, sectionY, 
                  prevMask, neighborMask,
                  prevMask != neighborMask ? "YES" : "NO");
    }
    
    void ClientChunkManager::ClearAllChunks() {
        ASSERT_MAIN_THREAD();
        m_chunks.clear();
        m_chunksWithDirtySections.clear();
        // Predictions reference positions in chunks that no longer exist —
        // drop them rather than letting a late ack roll back into a chunk
        // that has since been reloaded from scratch.
        m_prediction.Clear();
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
        if (::Render::g_chunkRenderer) {
            ::Render::g_chunkRenderer->MarkVisibleSectionsDirty();
        }
    }
    
    void ClientChunkManager::NotifyRenderGridChunkUnloaded(Game::Math::ChunkPos pos) {
        ASSERT_MAIN_THREAD();
        if (::Render::g_chunkRenderer) {
            ::Render::g_chunkRenderer->MarkVisibleSectionsDirty();
        }
    }
    
    void ClientChunkManager::NotifyRenderGridSectionUpdated(Game::Math::ChunkPos pos, int sectionY, 
                                                           ::Render::GPUSectionData* gpu) {
        ASSERT_MAIN_THREAD();
        // No-op: RenderGrid has been removed
    }

} // namespace Client