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
#include "../renderer/mesh/MeshUploadPermits.hpp"
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
    }

    void ClientChunkManager::MarkSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY) {
        ASSERT_MAIN_THREAD();
        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end() && it->second->state == ChunkState::LOADED) {
            // Increment version to trigger rebuild (Minecraft-style)
            auto& sectionInfo = it->second->sectionInfos[sectionY];
            sectionInfo.version++;
            sectionInfo.dirty = true;
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
                    // Copy block data directly
                    std::memcpy(section->blocks.data(), sectionBlocks, sectionSize);
                    
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
                                           uint8_t stateIndex) {
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
        MarkSectionDirty(chunkPos, sectionY);

        // Mark neighbor sections dirty when block is on a chunk/section boundary
        // (the neighbor's mesh depends on this block for face culling)
        int localY = (pos.y + 64) & 0xF;  // position within section (0-15)
        if (localX == 0)  MarkSectionDirty({chunkPos.x - 1, chunkPos.z}, sectionY);
        if (localX == 15) MarkSectionDirty({chunkPos.x + 1, chunkPos.z}, sectionY);
        if (localZ == 0)  MarkSectionDirty({chunkPos.x, chunkPos.z - 1}, sectionY);
        if (localZ == 15) MarkSectionDirty({chunkPos.x, chunkPos.z + 1}, sectionY);
        if (localY == 0  && sectionY > 0)  MarkSectionDirty(chunkPos, sectionY - 1);
        if (localY == 15 && sectionY < 23) MarkSectionDirty(chunkPos, sectionY + 1);
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
        SetBlockLocal(pos, newBlock, stateIndex);
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
                        // Decode palette/direct data into section->blocks[] directly.
                        // Blocks are stored in Y-Z-X order: index = y*256 + z*16 + x.
                        // The packed data iterates linearly through this index, so we
                        // write directly by index — no coordinate math needed.
                        bool decodedSuccessfully = false;
                        auto& blocks = section->blocks;

                        if (sectionData.bitsPerEntry == 0) {
                            // Single-value section — entire section is one block type
                            uint16_t blockId = (!sectionData.palette.empty())
                                ? static_cast<uint16_t>(sectionData.palette[0] & 0xFFFF)
                                : 0;
                            std::fill(blocks.begin(), blocks.end(), blockId);
                            decodedSuccessfully = true;

                        } else if (sectionData.bitsPerEntry <= 8) {
                            // Palette mode
                            if (!sectionData.palette.empty() && !sectionData.dataArray.empty()) {
                                const int bitsPerBlock = sectionData.bitsPerEntry;
                                const int blocksPerLong = 64 / bitsPerBlock;
                                const uint64_t mask = (1ULL << bitsPerBlock) - 1;
                                const size_t paletteSize = sectionData.palette.size();

                                int blockIndex = 0;
                                for (uint64_t packedLong : sectionData.dataArray) {
                                    for (int i = 0; i < blocksPerLong && blockIndex < 4096; ++i) {
                                        uint32_t paletteIndex = (packedLong >> (i * bitsPerBlock)) & mask;
                                        blocks[blockIndex] = (paletteIndex < paletteSize)
                                            ? static_cast<uint16_t>(sectionData.palette[paletteIndex] & 0xFFFF)
                                            : 0;
                                        blockIndex++;
                                    }
                                }
                                decodedSuccessfully = true;
                            }
                        } else {
                            // Direct mode (bitsPerEntry > 8) — global palette block state IDs
                            const int bitsPerBlock = std::max(15, static_cast<int>(sectionData.bitsPerEntry));
                            const int blocksPerLong = 64 / bitsPerBlock;
                            const uint64_t mask = (1ULL << bitsPerBlock) - 1;

                            int blockIndex = 0;
                            for (uint64_t packedLong : sectionData.dataArray) {
                                for (int i = 0; i < blocksPerLong && blockIndex < 4096; ++i) {
                                    blocks[blockIndex] = static_cast<uint16_t>(
                                        (packedLong >> (i * bitsPerBlock)) & mask);
                                    blockIndex++;
                                }
                            }
                            decodedSuccessfully = true;
                        }
                        
                        // Block-state plane. Absent means every voxel is at its
                        // block's default state, so drop any stale plane rather
                        // than leaving one behind from a previous occupant.
                        if (!sectionData.states.empty() && sectionData.states.size() == 4096) {
                            section->states = sectionData.states;
                        } else {
                            section->states.clear();
                        }

                        if (decodedSuccessfully) {
                            // Initialize section state properly (MC-style)
                            auto& sectionInfo = chunk->sectionInfos[y];
                            sectionInfo.hasCpuData = true;
                            sectionInfo.isAllAir = false;  // Has non-air blocks
                            sectionInfo.version++;        // 0→1 on first load, +1 on updates
                            sectionInfo.state = SectionState::LOADED;
                            sectionInfo.dirty = true;  // Needs meshing
                            
                            // Also update legacy dirty set for now (will remove later)
                            chunk->dirtySections.insert(y);
                            m_chunksWithDirtySections.insert(chunkPos);
                            
                            // Log::Debug("[mesh] loaded cx=%d cz=%d sy=%d ver=%u", 
                            //      chunkPos.x, chunkPos.z, y, sectionInfo.version);
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
        
        // Noise biomes, one id per 4x4x4 cell. Size-checked rather than trusted
        // so a malformed or version-skewed packet leaves the chunk with no
        // biome data (every lookup then answers with the fallback) instead of
        // handing the mesher a short array to index into.
        if (packet.biomes.size() == static_cast<size_t>(Game::Chunk::BIOME_COUNT)) {
            chunk->chunkData->biomes = packet.biomes;
            // One-shot: confirms biomes survive generation -> wire -> client.
            // Without it a silent break anywhere upstream just looks like
            // "every chunk is plains", which is indistinguishable from a
            // colour bug on screen.
            static bool s_loggedBiomes = false;
            if (!s_loggedBiomes) {
                s_loggedBiomes = true;
                std::unordered_map<uint16_t, int> hist;
                for (uint16_t b : packet.biomes) hist[b]++;
                std::string summary;
                for (const auto& [id, n] : hist) {
                    summary += " " + std::string(Game::BiomeRegistry::Get(id).name)
                             + "=" + std::to_string(n);
                }
                Log::Info("First chunk with biomes (%d, %d):%s",
                          chunkPos.x, chunkPos.z, summary.c_str());
            }
        } else if (packet.biomes.empty()) {
            static bool s_loggedNoBiomes = false;
            if (!s_loggedNoBiomes) {
                s_loggedNoBiomes = true;
                Log::Warning("Chunk (%d, %d) arrived with NO biome data - every tint "
                             "will resolve to the plains fallback",
                             chunkPos.x, chunkPos.z);
            }
        } else if (!packet.biomes.empty()) {
            Log::Warning("Chunk (%d, %d) carried %zu biome entries, expected %d - ignoring",
                         chunkPos.x, chunkPos.z, packet.biomes.size(),
                         Game::Chunk::BIOME_COUNT);
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
    void ClientChunkManager::ScheduleMeshBuildsWithSnapshots(const glm::vec3& playerPosition) {
        PROFILE_ZONE;
        ASSERT_MAIN_THREAD();

        // Throttle. This is the ceiling on how fast chunks can become visible:
        // each pass can start at most permits.Available() sections (10, matching
        // MC's SectionBufferBuilderPool sized at availableProcessors), so the
        // rate is permits x passes/sec.
        //
        // At the old 33ms (30 passes/sec) that capped us at ~300 sections/s
        // — measured 197/s — while chunks ARRIVED at ~570 sections/s. Meshing
        // took 72s to work through what the server delivered in 25s, and that
        // backlog is what shows up as chunks appearing slowly, long after the
        // data is already on the client. The mesh workers were 1.6% busy the
        // whole time; nothing was overloaded, we were just not asking.
        //
        // MC has no such throttle — LevelRenderer compiles sections every frame.
        // The period only exists so we do not rescan the dirty set at our full
        // frame rate; the permit pool above is the real backpressure, and when
        // uploads fall behind Available() hits 0 and this returns immediately.
        //
        // Size it against the actual demand, not a round number. Sections needing
        // a mesh per second is:
        //
        //     chunks/s (80.5) x sections/chunk (7.9) x remesh factor (2.2) ~ 1400/s
        //
        // and the ceiling is permits (10) x 1000/period. At 8ms that was 1250/s —
        // 11% BELOW demand, so meshing could never catch up and 17s of arrivals
        // took 37s to work through. 5ms gives 2000/s, ~43% headroom.
        //
        // The 2.2x remesh factor is NOT a bug to be optimised away: MC does the
        // same thing (ClientPacketListener.enableChunkLight ->
        // setSectionRangeDirty over the chunk AND its 8 neighbours, full Y range),
        // and gating compiles on neighbour availability would only make sections
        // appear LATER. Budget for it instead.
        static constexpr float kMeshSchedulePeriodMs = 5.0f;
        auto now = std::chrono::steady_clock::now();
        float elapsedMs = std::chrono::duration<float, std::milli>(now - m_lastMeshScheduleTime).count();
        if (elapsedMs < kMeshSchedulePeriodMs) {
            return;
        }
        m_lastMeshScheduleTime = now;

        auto workerPool = Threading::g_clientWorkerPool.get();
        if (!workerPool) return;

        // Buffer pool backpressure, MC-style (SectionBufferBuilderPool +
        // SectionRenderDispatcher.runTask:74). A permit spans compile AND
        // upload and only comes back once the render thread has uploaded the
        // result, so a slow upload stage throttles submission automatically.
        //
        // The previous check counted GetActiveJobCount() + GetPendingJobCount()
        // — both COMPILE-stage numbers. Results that had finished compiling and
        // were waiting to be uploaded counted as zero, so the result queue grew
        // without limit behind a check that read as "plenty of room", and the
        // render thread paid for the whole backlog at the swap.
        // ::Render, not Client::Render — this TU is inside namespace Client,
        // which has its own nested Render (MeshJobData et al).
        auto& permits = ::Render::GetMeshUploadPermits();
        size_t slotsAvailable = permits.Available();
        if (slotsAvailable == 0) return;

        // Collect dirty sections — reuse persistent buffer to avoid per-call heap allocation.
        // Iterates only the dirty-chunk index (not every loaded chunk); entries whose
        // chunk is gone or fully clean are lazily erased here.
        m_meshCandidates.clear();

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

            float dx = chunkPos.x * 16.0f + 8.0f - playerPosition.x;
            float dz = chunkPos.z * 16.0f + 8.0f - playerPosition.z;
            float xzDistSq = dx * dx + dz * dz;

            for (int sectionY : chunk->dirtySections) {
                if (sectionY < 0 || sectionY >= 24) continue;
                auto& si = chunk->sectionInfos[sectionY];
                if (!si.dirty) continue;
                if (si.meshingVersion == si.version) continue; // in flight

                float sectionCenterY = -64.0f + sectionY * 16.0f + 8.0f;
                float dy = sectionCenterY - playerPosition.y;
                // Squared distance with Y attenuated (0.1 factor squared = 0.01)
                float distSq = xzDistSq + dy * dy * 0.01f;

                // Initial compiles get 4x priority boost (0.5^2 = 0.25 on squared distance)
                float effectiveDistSq = (!si.builtOnce && !si.isAllAir) ? distSq * 0.25f : distSq;

                m_meshCandidates.push_back({chunkPos, sectionY, effectiveDistSq, chunk});
            }
        }

        // Sort by squared distance (monotonic, same order as sqrt)
        std::sort(m_meshCandidates.begin(), m_meshCandidates.end(),
                  [](const auto& a, const auto& b) { return a.effectiveDistSq < b.effectiveDistSq; });

        // Submit snapshots
        const size_t MAX_SNAPSHOTS_PER_FRAME = 128;
        size_t maxToSubmit = std::min(slotsAvailable, MAX_SNAPSHOTS_PER_FRAME);
        size_t sectionsSubmitted = 0;

        for (const auto& candidate : m_meshCandidates) {
            if (sectionsSubmitted >= maxToSubmit) break;

            auto* chunk = candidate.chunk;
            auto& sectionInfo = chunk->sectionInfos[candidate.sectionY];

            if (!sectionInfo.dirty || sectionInfo.meshingVersion == sectionInfo.version) continue;

            const uint32_t expectedVersion = sectionInfo.version;

            std::shared_ptr<Render::MeshJobData> snapshot;
            if (!BuildSectionSnapshot(candidate.chunkPos, candidate.sectionY, expectedVersion, snapshot)) {
                continue;
            }

            if (sectionInfo.isAllAir) {
                snapshot->jobType = Render::MeshJobType::BorderOnly;
            } else if (!sectionInfo.builtOnce) {
                snapshot->jobType = Render::MeshJobType::Initial;
            } else {
                snapshot->jobType = Render::MeshJobType::Full;
            }

            snapshot->distanceToPlayer = candidate.effectiveDistSq;
            snapshot->isHighPriority = (candidate.effectiveDistSq < 16384.0f); // 128^2
            snapshot->submitTime = std::chrono::steady_clock::now();

            // Claim the pipeline slot BEFORE submitting. Held until the render
            // thread has uploaded this section's result (or the result is
            // dropped/discarded), which is what makes the backpressure span
            // both stages the way MC's buffer pack does.
            if (!permits.TryAcquire()) break;   // pipeline full — retry next pass

            if (workerPool->SubmitMeshJobWithSnapshot(snapshot)) {
                if (sectionInfo.lastMeshJob) {
                    sectionInfo.lastMeshJob->Cancel();
                }
                sectionInfo.lastMeshJob = snapshot;
                sectionInfo.meshingVersion = expectedVersion;
                sectionInfo.state = SectionState::MESHING;
                sectionInfo.dirty = false;
                chunk->dirtySections.erase(candidate.sectionY);
                sectionsSubmitted++;
            } else {
                // Rejected before entering the pipeline (pool stopped, queue
                // full): nothing will ever produce a result for it, so the
                // permit has to go back here or it is leaked outright.
                permits.Release();
            }
        }

        if (sectionsSubmitted > 0) {
            Log::Debug("SCHEDULE: Submitted %zu mesh jobs (%zu slots, %zu candidates)",
                      sectionsSubmitted, slotsAvailable, m_meshCandidates.size());
        }
    }
    
    void ClientChunkManager::ScheduleDirtySectionMeshes() {
        // Get player position from somewhere (TODO: implement proper player tracking)
        glm::vec3 playerPos(0, 67, 0);
        ScheduleMeshBuildsWithSnapshots(playerPos);
    }
    
    // Helper: copy a 16x16 boundary plane from a neighbor section into the snapshot.
    // face: 0=north(z=15), 1=south(z=0), 2=east(x=0), 3=west(x=15), 4=up(y=0), 5=down(y=15)
    static void CopyNeighborBoundaryPlane(
        const Game::ChunkSection* neighborSection, int face,
        std::array<Game::BlockID, 256>& outPlane) {

        if (!neighborSection || neighborSection->blocks.size() < 4096) {
            std::fill(outPlane.begin(), outPlane.end(), Game::BlockID::Air);
            return;
        }

        const auto& blocks = neighborSection->blocks;

        switch (face) {
            case 0: // North: z=15 plane → indexed as [y*16+x]
                for (int y = 0; y < 16; ++y)
                    std::memcpy(&outPlane[y * 16], &blocks[y * 256 + 15 * 16], 16 * sizeof(uint16_t));
                break;
            case 1: // South: z=0 plane → indexed as [y*16+x]
                for (int y = 0; y < 16; ++y)
                    std::memcpy(&outPlane[y * 16], &blocks[y * 256], 16 * sizeof(uint16_t));
                break;
            case 2: // East: x=0 plane → indexed as [y*16+z]
                for (int y = 0; y < 16; ++y)
                    for (int z = 0; z < 16; ++z)
                        outPlane[y * 16 + z] = static_cast<Game::BlockID>(blocks[y * 256 + z * 16 + 0]);
                break;
            case 3: // West: x=15 plane → indexed as [y*16+z]
                for (int y = 0; y < 16; ++y)
                    for (int z = 0; z < 16; ++z)
                        outPlane[y * 16 + z] = static_cast<Game::BlockID>(blocks[y * 256 + z * 16 + 15]);
                break;
            case 4: // Up: y=0 plane → indexed as [z*16+x]
                std::memcpy(outPlane.data(), blocks.data(), 256 * sizeof(uint16_t));
                break;
            case 5: // Down: y=15 plane → indexed as [z*16+x]
                std::memcpy(outPlane.data(), &blocks[15 * 256], 256 * sizeof(uint16_t));
                break;
        }
    }

    bool ClientChunkManager::BuildSectionSnapshot(
        Game::Math::ChunkPos chunkPos, int sectionY,
        uint32_t expectedVersion,
        std::shared_ptr<Render::MeshJobData>& outSnapshot) {

        PROFILE_ZONE_N("BuildSnapshot");
        ASSERT_MAIN_THREAD();

        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end() || !it->second || !it->second->chunkData) {
            return false;
        }

        auto& chunk = it->second;
        auto& sectionInfo = chunk->sectionInfos[sectionY];

        if (!sectionInfo.hasCpuData) return false;
        if (sectionInfo.version != expectedVersion) return false;

        auto* section = chunk->chunkData->GetSection(sectionY);

        outSnapshot = std::make_shared<Render::MeshJobData>(chunkPos, sectionY);
        outSnapshot->generation = expectedVersion;
        outSnapshot->sectionData.sectionY = sectionY;

        // Copy center section blocks via memcpy (BlockID is uint16_t, same as blocks[])
        if (section) {
            std::memcpy(outSnapshot->sectionData.blocks.data(),
                       section->blocks.data(), 4096 * sizeof(uint16_t));
            // Empty check via memcmp against a zero page — vectorized by libc,
            // and it still early-outs on the first non-Air block.
            static const uint16_t kZeroSection[4096] = {};
            outSnapshot->sectionData.isEmpty =
                std::memcmp(section->blocks.data(), kZeroSection, 4096 * sizeof(uint16_t)) == 0;

            // Block states, only when the section actually has any. Both sides
            // stay unallocated for ordinary terrain (see ChunkSection::states),
            // so this costs a branch per snapshot in the common case.
            if (section->HasStates()) {
                outSnapshot->sectionData.states = section->states;
            } else {
                outSnapshot->sectionData.states.clear();
            }

            // Biomes for this section plus the blend margin. The margin reaches
            // into the four neighbouring chunks, so each cell is resolved
            // through the chunk map rather than off `chunk` alone — a border
            // section otherwise blends against its own biome on one side and
            // produces a visible seam the vanilla client does not have.
            auto& snapBiomes = outSnapshot->sectionData;
            snapBiomes.hasBiomes = false;
            if (!chunk->chunkData->biomes.empty()) {
                using SS = Render::SectionSnapshot;
                const int baseX = chunkPos.x * Game::Math::CHUNK_SIZE_X;
                const int baseZ = chunkPos.z * Game::Math::CHUNK_SIZE_Z;
                const int baseY = Game::Math::WorldCoordinates::SectionCoordsToWorldY(sectionY, 0);

                for (int qy = 0; qy < SS::BIOME_Y; ++qy) {
                    for (int qz = 0; qz < SS::BIOME_XZ; ++qz) {
                        for (int qx = 0; qx < SS::BIOME_XZ; ++qx) {
                            // Grid cell 0 is quart cell -1, i.e. 4 blocks before
                            // the section's own origin.
                            const int wx = baseX + (qx - 1) * 4;
                            const int wz = baseZ + (qz - 1) * 4;
                            const int wy = baseY + qy * 4;
                            snapBiomes.biomes[
                                static_cast<size_t>((qy * SS::BIOME_XZ + qz) * SS::BIOME_XZ + qx)] =
                                    BiomeAtWorld(wx, wy, wz);
                        }
                    }
                }
                snapBiomes.hasBiomes = true;
            }
        } else {
            outSnapshot->sectionData.isEmpty = true;
            std::memset(outSnapshot->sectionData.blocks.data(), 0, 4096 * sizeof(uint16_t));
        }

        // Copy boundary planes from 6 neighbors (256 blocks each, not full 4096)
        uint8_t neighborMask = 0;

        // North (-z): need z=15 plane from neighbor
        auto northIt = m_chunks.find({chunkPos.x, chunkPos.z - 1});
        if (northIt != m_chunks.end() && northIt->second && northIt->second->chunkData) {
            neighborMask |= 8;
            CopyNeighborBoundaryPlane(northIt->second->chunkData->GetSection(sectionY), 0,
                                      outSnapshot->sectionData.neighbors[0]);
        } else {
            std::fill(outSnapshot->sectionData.neighbors[0].begin(),
                     outSnapshot->sectionData.neighbors[0].end(), Game::BlockID::Air);
        }

        // South (+z): need z=0 plane from neighbor
        auto southIt = m_chunks.find({chunkPos.x, chunkPos.z + 1});
        if (southIt != m_chunks.end() && southIt->second && southIt->second->chunkData) {
            neighborMask |= 4;
            CopyNeighborBoundaryPlane(southIt->second->chunkData->GetSection(sectionY), 1,
                                      outSnapshot->sectionData.neighbors[1]);
        } else {
            std::fill(outSnapshot->sectionData.neighbors[1].begin(),
                     outSnapshot->sectionData.neighbors[1].end(), Game::BlockID::Air);
        }

        // East (+x): need x=0 plane from neighbor
        auto eastIt = m_chunks.find({chunkPos.x + 1, chunkPos.z});
        if (eastIt != m_chunks.end() && eastIt->second && eastIt->second->chunkData) {
            neighborMask |= 1;
            CopyNeighborBoundaryPlane(eastIt->second->chunkData->GetSection(sectionY), 2,
                                      outSnapshot->sectionData.neighbors[2]);
        } else {
            std::fill(outSnapshot->sectionData.neighbors[2].begin(),
                     outSnapshot->sectionData.neighbors[2].end(), Game::BlockID::Air);
        }

        // West (-x): need x=15 plane from neighbor
        auto westIt = m_chunks.find({chunkPos.x - 1, chunkPos.z});
        if (westIt != m_chunks.end() && westIt->second && westIt->second->chunkData) {
            neighborMask |= 2;
            CopyNeighborBoundaryPlane(westIt->second->chunkData->GetSection(sectionY), 3,
                                      outSnapshot->sectionData.neighbors[3]);
        } else {
            std::fill(outSnapshot->sectionData.neighbors[3].begin(),
                     outSnapshot->sectionData.neighbors[3].end(), Game::BlockID::Air);
        }

        // Up (+y): need y=0 plane from section above
        if (sectionY < 23) {
            CopyNeighborBoundaryPlane(chunk->chunkData->GetSection(sectionY + 1), 4,
                                      outSnapshot->sectionData.neighbors[4]);
        } else {
            std::fill(outSnapshot->sectionData.neighbors[4].begin(),
                     outSnapshot->sectionData.neighbors[4].end(), Game::BlockID::Air);
        }

        // Down (-y): need y=15 plane from section below
        if (sectionY > 0) {
            CopyNeighborBoundaryPlane(chunk->chunkData->GetSection(sectionY - 1), 5,
                                      outSnapshot->sectionData.neighbors[5]);
        } else {
            std::fill(outSnapshot->sectionData.neighbors[5].begin(),
                     outSnapshot->sectionData.neighbors[5].end(), Game::BlockID::Air);
        }

        // Fill light data with default values
        std::memset(outSnapshot->sectionData.lightData.data(), 0xFF,
                   outSnapshot->sectionData.lightData.size());

        outSnapshot->neighborMask = neighborMask;

        // Final version check
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