// File: src/client/world/ClientChunkManager.cpp
#include "ClientChunkManager.hpp"
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
        Log::Info("CLIENT UNLOAD: chunk (%d, %d)", chunkPos.x, chunkPos.z);

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
        ASSERT_MAIN_THREAD();
        Log::Debug("Processing ChunkDataS2CPacket for chunk (%d, %d)", packet.chunkX, packet.chunkZ);
        
        // Use ApplyChunkData for proper handling
        Game::Math::ChunkPos chunkPos{packet.chunkX, packet.chunkZ};
        ApplyChunkData(chunkPos, packet);
    }
    
    // Process block change packet
    void ClientChunkManager::ProcessBlockChange(const Network::BlockChangeS2CPacket& packet) {
        ASSERT_MAIN_THREAD();
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
        
        // Calculate local block position within chunk
        int localX = packet.worldX & 0xF;  // mod 16
        int localZ = packet.worldZ & 0xF;
        int sectionY = (packet.worldY + 64) >> 4;  // Convert to section index
        
        // Set the block in the chunk
        chunk->chunkData->SetBlock(localX, packet.worldY, localZ, packet.newBlockId);
        
        // Mark section as dirty for remeshing
        MarkSectionDirty(chunkPos, sectionY);
        
        Log::Debug("Block change at (%d, %d, %d) in chunk (%d, %d) to block %d",
                  packet.worldX, packet.worldY, packet.worldZ, 
                  chunkPos.x, chunkPos.z, static_cast<int>(packet.newBlockId));
    }
    
    void ClientChunkManager::ApplyChunkData(Game::Math::ChunkPos chunkPos, const Network::ChunkDataS2CPacket& packet) {
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
                        // Decode palette and data array into block IDs
                        bool decodedSuccessfully = false;
                        
                        if (sectionData.bitsPerEntry > 0 && sectionData.bitsPerEntry <= 8) {
                            // Palette mode - decode using palette
                            if (!sectionData.palette.empty() && !sectionData.dataArray.empty()) {
                                int bitsPerBlock = sectionData.bitsPerEntry;
                                int blocksPerLong = 64 / bitsPerBlock;
                                uint64_t mask = (1ULL << bitsPerBlock) - 1;
                                
                                // Decode packed data into block IDs
                                int blockIndex = 0;
                                for (uint64_t packedLong : sectionData.dataArray) {
                                    for (int i = 0; i < blocksPerLong && blockIndex < 4096; ++i) {
                                        uint32_t paletteIndex = (packedLong >> (i * bitsPerBlock)) & mask;
                                        
                                        // Get block ID from palette
                                        uint16_t blockId = 0; // Default to air
                                        if (paletteIndex < sectionData.palette.size()) {
                                            // For now, just use the palette value directly
                                            // In a real implementation, this would map Minecraft block state IDs to our BlockID enum
                                            blockId = static_cast<uint16_t>(sectionData.palette[paletteIndex] & 0xFFFF);
                                        }
                                        
                                        // Calculate block position within section
                                        int localY = blockIndex / 256;
                                        int localZ = (blockIndex % 256) / 16;
                                        int localX = blockIndex % 16;
                                        
                                        // Set the block in the section
                                        section->Set(localX, localY, localZ, blockId);
                                        blockIndex++;
                                    }
                                }
                                decodedSuccessfully = true;
                            }
                        } else if (sectionData.bitsPerEntry > 8) {
                            // Direct mode - block IDs are stored directly
                            // For Minecraft 1.16+, this means global palette (block state IDs)
                            int bitsPerBlock = std::max(15, static_cast<int>(sectionData.bitsPerEntry)); // Minecraft uses at least 15 bits for direct
                            int blocksPerLong = 64 / bitsPerBlock;
                            uint64_t mask = (1ULL << bitsPerBlock) - 1;
                            
                            int blockIndex = 0;
                            for (uint64_t packedLong : sectionData.dataArray) {
                                for (int i = 0; i < blocksPerLong && blockIndex < 4096; ++i) {
                                    uint32_t blockStateId = (packedLong >> (i * bitsPerBlock)) & mask;
                                    
                                    // Map Minecraft block state ID to our BlockID
                                    // For now, use a simple mapping
                                    uint16_t blockId = 0; // Default to air
                                    if (blockStateId > 0) {
                                        // Very simplified mapping - in reality this needs a proper block state registry
                                        blockId = static_cast<uint16_t>(blockStateId);
                                    }
                                    
                                    // Calculate block position within section
                                    int localY = blockIndex / 256;
                                    int localZ = (blockIndex % 256) / 16;
                                    int localX = blockIndex % 16;
                                    
                                    // Set the block in the section
                                    section->Set(localX, localY, localZ, blockId);
                                    blockIndex++;
                                }
                            }
                            decodedSuccessfully = true;
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
        
        // Copy biome data if groundUp
        if (packet.groundUpContinuous && !packet.biomeData.empty()) {
            // TODO: Store biome data in chunk
            Log::Debug("Applied biome data to chunk (%d, %d)", chunkPos.x, chunkPos.z);
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
                
                chunk->chunkData->SetBlock(localX, blockPos.y, localZ, change.blockId);
                chunk->dirtySections.insert(sectionY);
                
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

        // Throttle: run at most every 33ms (~30 times/sec) to avoid
        // iterating all chunks and computing sqrt() every single frame
        auto now = std::chrono::steady_clock::now();
        float elapsedMs = std::chrono::duration<float, std::milli>(now - m_lastMeshScheduleTime).count();
        if (elapsedMs < 33.0f) {
            return;
        }
        m_lastMeshScheduleTime = now;

        auto workerPool = Threading::g_clientWorkerPool.get();
        if (!workerPool) return;

        // Buffer pool backpressure: only submit when pipeline has room
        // POOL_SIZE = workerCount * 8 (keeps workers fed between frames)
        size_t activeAndPending = workerPool->GetActiveJobCount() + workerPool->GetPendingJobCount();
        const size_t POOL_SIZE = std::max(size_t(16), workerPool->GetWorkerCount() * 8);
        if (activeAndPending >= POOL_SIZE) return;
        size_t slotsAvailable = POOL_SIZE - activeAndPending;

        // Collect dirty sections that need meshing
        struct SectionCandidate {
            Game::Math::ChunkPos chunkPos;
            int sectionY;
            float effectiveDistance; // initials get 0.5x bonus (sorts ahead of recompiles)
        };
        std::vector<SectionCandidate> candidates;

        for (auto& [chunkPos, chunk] : m_chunks) {
            if (!chunk || !chunk->chunkData) continue;

            float centerX = chunkPos.x * 16.0f + 8.0f;
            float centerZ = chunkPos.z * 16.0f + 8.0f;

            for (int sectionY = 0; sectionY < 24; ++sectionY) {
                auto& si = chunk->sectionInfos[sectionY];
                if (!si.dirty) continue;
                if (si.meshingVersion == si.version) continue; // in flight

                bool needsProcessing = !si.isAllAir || !si.builtOnce || si.dirty;
                if (!needsProcessing) {
                    si.dirty = false;
                    chunk->dirtySections.erase(sectionY);
                    continue;
                }

                float xzDist = std::sqrt((centerX - playerPosition.x) * (centerX - playerPosition.x) +
                                         (centerZ - playerPosition.z) * (centerZ - playerPosition.z));
                float sectionCenterY = -64.0f + sectionY * 16.0f + 8.0f;
                float yDist = std::abs(sectionCenterY - playerPosition.y);
                float distance = xzDist + yDist * 0.1f;

                // Initial compiles (never built) get 2x priority boost
                // Matches Minecraft's CompileTaskDynamicQueue: initials beat recompiles
                float effectiveDist = (!si.builtOnce && !si.isAllAir) ? distance * 0.5f : distance;

                candidates.push_back({chunkPos, sectionY, effectiveDist});
            }
        }

        // Sort by effective distance (initials float to top due to 0.5x bonus)
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.effectiveDistance < b.effectiveDistance; });

        // Submit up to min(slotsAvailable, 64) — fixed cap prevents pathological snapshot cost
        const size_t MAX_SNAPSHOTS_PER_FRAME = 64;
        size_t maxToSubmit = std::min(slotsAvailable, MAX_SNAPSHOTS_PER_FRAME);
        size_t sectionsSubmitted = 0;

        for (const auto& candidate : candidates) {
            if (sectionsSubmitted >= maxToSubmit) break;

            auto it = m_chunks.find(candidate.chunkPos);
            if (it == m_chunks.end()) continue;

            auto& chunk = it->second;
            auto& sectionInfo = chunk->sectionInfos[candidate.sectionY];

            if (!sectionInfo.dirty || sectionInfo.meshingVersion == sectionInfo.version) continue;

            const uint32_t expectedVersion = sectionInfo.version;

            std::shared_ptr<Render::MeshJobData> snapshot;
            if (!BuildSectionSnapshot(candidate.chunkPos, candidate.sectionY, expectedVersion, snapshot)) {
                continue;
            }

            // Set job type: Initial (never built) > Full (recompile) > BorderOnly (empty)
            if (sectionInfo.isAllAir) {
                snapshot->jobType = Render::MeshJobType::BorderOnly;
            } else if (!sectionInfo.builtOnce) {
                snapshot->jobType = Render::MeshJobType::Initial;
            } else {
                snapshot->jobType = Render::MeshJobType::Full;
            }

            snapshot->distanceToPlayer = candidate.effectiveDistance;
            snapshot->isHighPriority = (candidate.effectiveDistance < 128.0f);
            snapshot->submitTime = std::chrono::steady_clock::now();

            if (workerPool->SubmitMeshJobWithSnapshot(snapshot)) {
                // Cancel previous in-flight task only after successful submission
                // (avoids cancelling old job when new one can't be queued)
                if (sectionInfo.lastMeshJob) {
                    sectionInfo.lastMeshJob->Cancel();
                }
                sectionInfo.lastMeshJob = snapshot; // Track for cancellation
                sectionInfo.meshingVersion = expectedVersion;
                sectionInfo.state = SectionState::MESHING;
                sectionInfo.dirty = false;
                chunk->dirtySections.erase(candidate.sectionY);
                sectionsSubmitted++;
            }
            // If submission failed (queue full), dirty flag stays true
            // and meshingVersion is NOT updated, so it will retry next cycle
        }

        if (sectionsSubmitted > 0) {
            Log::Debug("SCHEDULE: Submitted %zu mesh jobs (%zu slots, %zu candidates)",
                      sectionsSubmitted, slotsAvailable, candidates.size());
        }
    }
    
    void ClientChunkManager::ScheduleDirtySectionMeshes() {
        // Get player position from somewhere (TODO: implement proper player tracking)
        glm::vec3 playerPos(0, 67, 0);
        ScheduleMeshBuildsWithSnapshots(playerPos);
    }
    
    bool ClientChunkManager::BuildSectionSnapshot(
        Game::Math::ChunkPos chunkPos, int sectionY, 
        uint32_t expectedVersion,
        std::shared_ptr<Render::MeshJobData>& outSnapshot) {
        
        ASSERT_MAIN_THREAD();
        
        // Direct access to chunks - no locks needed on main thread
        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end() || !it->second || !it->second->chunkData) {
            return false; // No chunk data
        }
        
        auto& chunk = it->second;
        auto& sectionInfo = chunk->sectionInfos[sectionY];
        
        // Check if we have CPU data for this section
        if (!sectionInfo.hasCpuData) {
            return false; // No data available
        }
        
        // Check version before reading (optimistic read)
        if (sectionInfo.version != expectedVersion) {
            return false; // Version already changed
        }
        
        // Get section data (may be null for empty sections)
        auto* section = chunk->chunkData->GetSection(sectionY);
        
        // Create snapshot
        outSnapshot = std::make_shared<Render::MeshJobData>(chunkPos, sectionY);
        outSnapshot->generation = expectedVersion;
        
        // Compute neighbor chunk presence mask (PX=1, NX=2, PZ=4, NZ=8)
        uint8_t neighborMask = 0;
        
        // Copy center section blocks
        outSnapshot->sectionData.sectionY = sectionY;
        
        if (section) {
            // Section has data - copy blocks
            outSnapshot->sectionData.isEmpty = true;
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        int index = y * 256 + z * 16 + x;
                        Game::BlockID block = static_cast<Game::BlockID>(section->blocks[index]);
                        outSnapshot->sectionData.blocks[index] = block;
                        
                        if (block != Game::BlockID::Air) {
                            outSnapshot->sectionData.isEmpty = false;
                        }
                    }
                }
            }
        } else {
            // Empty section - fill with air
            outSnapshot->sectionData.isEmpty = true;
            std::fill(outSnapshot->sectionData.blocks.begin(), 
                     outSnapshot->sectionData.blocks.end(), 
                     Game::BlockID::Air);
        }
        
        // Always copy neighbor data for 1-voxel halo (even for empty sections)
        // This ensures proper face culling at chunk boundaries
        
        // Copy neighbor data directly (no GetChunk calls, direct map access)
        // Face indices: 0=north(-z), 1=south(+z), 2=east(+x), 3=west(-x), 4=up(+y), 5=down(-y)
        
        // North neighbor (-z)
        auto northIt = m_chunks.find({chunkPos.x, chunkPos.z - 1});
        if (northIt != m_chunks.end() && northIt->second && northIt->second->chunkData) {
            neighborMask |= 8;  // NZ bit
            auto* neighborSection = northIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    outSnapshot->sectionData.neighbors[0][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(outSnapshot->sectionData.neighbors[0].begin(), 
                         outSnapshot->sectionData.neighbors[0].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(outSnapshot->sectionData.neighbors[0].begin(), 
                     outSnapshot->sectionData.neighbors[0].end(), Game::BlockID::Air);
        }
        
        // South neighbor (+z)
        auto southIt = m_chunks.find({chunkPos.x, chunkPos.z + 1});
        if (southIt != m_chunks.end() && southIt->second && southIt->second->chunkData) {
            neighborMask |= 4;  // PZ bit
            auto* neighborSection = southIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    outSnapshot->sectionData.neighbors[1][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(outSnapshot->sectionData.neighbors[1].begin(), 
                         outSnapshot->sectionData.neighbors[1].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(outSnapshot->sectionData.neighbors[1].begin(), 
                     outSnapshot->sectionData.neighbors[1].end(), Game::BlockID::Air);
        }
        
        // East neighbor (+x)
        auto eastIt = m_chunks.find({chunkPos.x + 1, chunkPos.z});
        if (eastIt != m_chunks.end() && eastIt->second && eastIt->second->chunkData) {
            neighborMask |= 1;  // PX bit
            auto* neighborSection = eastIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    outSnapshot->sectionData.neighbors[2][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(outSnapshot->sectionData.neighbors[2].begin(), 
                         outSnapshot->sectionData.neighbors[2].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(outSnapshot->sectionData.neighbors[2].begin(), 
                     outSnapshot->sectionData.neighbors[2].end(), Game::BlockID::Air);
        }
        
        // West neighbor (-x)
        auto westIt = m_chunks.find({chunkPos.x - 1, chunkPos.z});
        if (westIt != m_chunks.end() && westIt->second && westIt->second->chunkData) {
            neighborMask |= 2;  // NX bit
            auto* neighborSection = westIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    outSnapshot->sectionData.neighbors[3][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(outSnapshot->sectionData.neighbors[3].begin(), 
                         outSnapshot->sectionData.neighbors[3].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(outSnapshot->sectionData.neighbors[3].begin(), 
                     outSnapshot->sectionData.neighbors[3].end(), Game::BlockID::Air);
        }
        
        // Up neighbor (same chunk, section above)
        if (sectionY < 23) {
            auto* upSection = chunk->chunkData->GetSection(sectionY + 1);
            if (upSection) {
                for (int i = 0; i < 4096; ++i) {
                    outSnapshot->sectionData.neighbors[4][i] = static_cast<Game::BlockID>(upSection->blocks[i]);
                }
            } else {
                std::fill(outSnapshot->sectionData.neighbors[4].begin(), 
                         outSnapshot->sectionData.neighbors[4].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(outSnapshot->sectionData.neighbors[4].begin(), 
                     outSnapshot->sectionData.neighbors[4].end(), Game::BlockID::Air);
        }
        
        // Down neighbor (same chunk, section below)
        if (sectionY > 0) {
            auto* downSection = chunk->chunkData->GetSection(sectionY - 1);
            if (downSection) {
                for (int i = 0; i < 4096; ++i) {
                    outSnapshot->sectionData.neighbors[5][i] = static_cast<Game::BlockID>(downSection->blocks[i]);
                }
            } else {
                std::fill(outSnapshot->sectionData.neighbors[5].begin(), 
                         outSnapshot->sectionData.neighbors[5].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(outSnapshot->sectionData.neighbors[5].begin(), 
                     outSnapshot->sectionData.neighbors[5].end(), Game::BlockID::Air);
        }
        
        // Fill light data with default values
        std::fill(outSnapshot->sectionData.lightData.begin(), 
                 outSnapshot->sectionData.lightData.end(), 0xFF);
        
        // Set the neighbor mask we computed based on actual chunk presence
        outSnapshot->neighborMask = neighborMask;
        
        // Final version check to ensure nothing changed during capture
        if (sectionInfo.version != expectedVersion) {
            return false; // Version changed during capture, retry next frame
        }
        
        return true;
    }
    
    MeshAcceptance ClientChunkManager::AcceptMeshResult(const Network::MeshBuildResult& result) {
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
        // No-op: RenderGrid has been removed
    }
    
    void ClientChunkManager::NotifyRenderGridChunkUnloaded(Game::Math::ChunkPos pos) {
        ASSERT_MAIN_THREAD();
        // No-op: RenderGrid has been removed
    }
    
    void ClientChunkManager::NotifyRenderGridSectionUpdated(Game::Math::ChunkPos pos, int sectionY, 
                                                           ::Render::GPUSectionData* gpu) {
        ASSERT_MAIN_THREAD();
        // No-op: RenderGrid has been removed
    }

} // namespace Client