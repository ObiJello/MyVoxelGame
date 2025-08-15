// File: src/client/world/ClientChunkManager.cpp
#include "ClientChunkManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "ClientWorkerPool.hpp"
#include "../renderer/core/Frustum.hpp"
#include "../renderer/mesh/MeshJobData.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
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

    void ClientChunkManager::ProcessChunkLoadPacket(const Network::ServerChunkDataPacket& packet) {
        ASSERT_MAIN_THREAD();
        Log::Debug("PIPELINE: ProcessChunkLoadPacket START for chunk (%d, %d)", 
                  packet.position.x, packet.position.z);
        
        if (!packet.chunkData) {
            Log::Warning("PIPELINE: Received ServerChunkDataPacket with null chunk data for (%d, %d)", 
                        packet.position.x, packet.position.z);
            return;
        }
        
        // Delegate to LoadChunk helper
        LoadChunk(packet.position, *packet.chunkData);
        
        Log::Debug("PIPELINE: ProcessChunkLoadPacket COMPLETE for chunk (%d, %d)", 
                  packet.position.x, packet.position.z);
    }
    
    void ClientChunkManager::LoadChunk(Game::Math::ChunkPos chunkPos, const Network::SerializedChunkData& serializedData) {
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
            
            if (chunk->chunkData->HasSection(sectionY)) {
                // Section has data
                sectionInfo.hasCpuData = true;
                sectionInfo.isAllAir = false;  // Assume non-empty if it has a section
                sectionInfo.state = SectionState::LOADED;
                sectionInfo.version++;
                sectionInfo.dirty = true;  // Needs meshing
                
                // Also update legacy dirty set for compatibility
                chunk->dirtySections.insert(sectionY);
            } else {
                // Section is empty (all air)
                sectionInfo.hasCpuData = true;  // We know it's air
                sectionInfo.isAllAir = true;
                sectionInfo.state = SectionState::LOADED;
                sectionInfo.version++;
                sectionInfo.dirty = false;  // No need to mesh empty sections
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

    // Unload chunk from packet
    void ClientChunkManager::ProcessChunkUnloadPacket(const Network::ServerChunkUnloadPacket& packet) {
        ASSERT_MAIN_THREAD();
        Log::Debug("Processing chunk unload packet for chunk (%d, %d)", 
                  packet.position.x, packet.position.z);

        // Unload immediately
        UnloadChunk(packet.position);
        
        Log::Debug("Chunk (%d, %d) unloaded", packet.position.x, packet.position.z);
    }

    void ClientChunkManager::UnloadChunk(Game::Math::ChunkPos chunkPos) {
        ASSERT_MAIN_THREAD();
        
        // Mark neighbor chunks' sections as dirty BEFORE unloading
        // This ensures they rebuild their meshes to show previously culled faces
        MarkNeighborSectionsDirty(chunkPos);
        
        // Cancel mesh jobs for this chunk
        Threading::CancelClientMeshJob(chunkPos);
        
        // Drop any pending diffs for this chunk
        if (m_pendingDiffs) {
            m_pendingDiffs->DropChunkDiffs(chunkPos);
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
        ASSERT_MAIN_THREAD();
        
        Log::Debug("=== MarkNeighborSectionsDirty for chunk (%d, %d) ===", chunkPos.x, chunkPos.z);
        
        // Only mark non-empty sections of adjacent chunks as dirty
        // This is much more efficient than marking all sections
        
        // North neighbor (-z)
        auto northPos = Game::Math::ChunkPos{chunkPos.x, chunkPos.z - 1};
        auto northIt = m_chunks.find(northPos);
        if (northIt != m_chunks.end() && northIt->second->state == ChunkState::LOADED) {
            int dirtyCount = 0;
            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                auto& sectionInfo = northIt->second->sectionInfos[sectionY];
                // Only mark non-empty sections that aren't already dirty
                if (!sectionInfo.isAllAir && !sectionInfo.dirty) {
                    sectionInfo.version++;  // Increment version (will cause in-flight meshes to be dropped)
                    sectionInfo.dirty = true;
                    northIt->second->dirtySections.insert(sectionY);  // Legacy support
                    dirtyCount++;
                }
            }
            if (dirtyCount > 0) {
                Log::Debug("  North neighbor (%d, %d): marked %d non-empty sections dirty", 
                          northPos.x, northPos.z, dirtyCount);
            }
        }
        
        // South neighbor (+z)
        auto southPos = Game::Math::ChunkPos{chunkPos.x, chunkPos.z + 1};
        auto southIt = m_chunks.find(southPos);
        if (southIt != m_chunks.end() && southIt->second->state == ChunkState::LOADED) {
            int dirtyCount = 0;
            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                auto& sectionInfo = southIt->second->sectionInfos[sectionY];
                // Only mark non-empty sections that aren't already dirty
                if (!sectionInfo.isAllAir && !sectionInfo.dirty) {
                    sectionInfo.version++;
                    sectionInfo.dirty = true;
                    southIt->second->dirtySections.insert(sectionY);
                    dirtyCount++;
                }
            }
            if (dirtyCount > 0) {
                Log::Debug("  South neighbor (%d, %d): marked %d non-empty sections dirty",
                          southPos.x, southPos.z, dirtyCount);
            }
        }
        
        // East neighbor (+x)
        auto eastPos = Game::Math::ChunkPos{chunkPos.x + 1, chunkPos.z};
        auto eastIt = m_chunks.find(eastPos);
        if (eastIt != m_chunks.end() && eastIt->second->state == ChunkState::LOADED) {
            int dirtyCount = 0;
            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                auto& sectionInfo = eastIt->second->sectionInfos[sectionY];
                // Only mark non-empty sections that aren't already dirty
                if (!sectionInfo.isAllAir && !sectionInfo.dirty) {
                    sectionInfo.version++;
                    sectionInfo.dirty = true;
                    eastIt->second->dirtySections.insert(sectionY);
                    dirtyCount++;
                }
            }
            if (dirtyCount > 0) {
                Log::Debug("  East neighbor (%d, %d): marked %d non-empty sections dirty",
                          eastPos.x, eastPos.z, dirtyCount);
            }
        }
        
        // West neighbor (-x)
        auto westPos = Game::Math::ChunkPos{chunkPos.x - 1, chunkPos.z};
        auto westIt = m_chunks.find(westPos);
        if (westIt != m_chunks.end() && westIt->second->state == ChunkState::LOADED) {
            int dirtyCount = 0;
            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                auto& sectionInfo = westIt->second->sectionInfos[sectionY];
                // Only mark non-empty sections that aren't already dirty
                if (!sectionInfo.isAllAir && !sectionInfo.dirty) {
                    sectionInfo.version++;
                    sectionInfo.dirty = true;
                    westIt->second->dirtySections.insert(sectionY);
                    dirtyCount++;
                }
            }
            if (dirtyCount > 0) {
                Log::Debug("  West neighbor (%d, %d): marked %d non-empty sections dirty",
                          westPos.x, westPos.z, dirtyCount);
            }
        }
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
        
        chunk->state = newState;
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
                                        blockId = static_cast<uint16_t>(blockStateId & 0xFF);
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
        ASSERT_MAIN_THREAD();
        
        // Check if worker pool is available
        auto workerPool = Threading::g_clientWorkerPool.get();
        if (!workerPool) {
            Log::Warning("SCHEDULE: No worker pool available for mesh builds");
            return;
        }
        
        // Step 1: Collect sections that need meshing
        struct SectionCandidate {
            Game::Math::ChunkPos chunkPos;
            int sectionY;
            float priority;
        };
        std::vector<SectionCandidate> sectionCandidates;

        for (auto& [chunkPos, chunk] : m_chunks) {
            if (!chunk || !chunk->chunkData) {
                continue;
            }
            
            // Calculate chunk center distance for priority
            float centerX = chunkPos.x * 16.0f + 8.0f;
            float centerZ = chunkPos.z * 16.0f + 8.0f;
            
            // Check each section
            for (int sectionY = 0; sectionY < 24; ++sectionY) {
                auto& sectionInfo = chunk->sectionInfos[sectionY];
                
                // Minecraft-style: use per-section dirty flag as source of truth
                // dirtySections set is just an index for iteration
                bool isDirty = sectionInfo.dirty;
                bool notInFlight = (sectionInfo.meshingVersion != sectionInfo.version);
                
                if (isDirty && notInFlight) {
                    // Determine if we need to process this section
                    bool needsProcessing = false;
                    
                    if (sectionInfo.isAllAir) {
                        // Empty section - only process if never built or neighbors might have changed
                        if (!sectionInfo.builtOnce || sectionInfo.dirty) {
                            needsProcessing = true;  // Will use BorderOnly job type
                        }
                    } else {
                        // Non-empty section always needs processing when dirty
                        needsProcessing = true;
                    }
                    
                    if (!needsProcessing) {
                        // Clear dirty flag for empty sections that don't need processing
                        sectionInfo.dirty = false;
                        chunk->dirtySections.erase(sectionY);
                        continue;
                    }
                    // Calculate section-specific priority using XZ distance only
                    // We don't filter by distance - if the chunk is loaded, all sections should be processed
                    float xzDistance = std::sqrt((centerX - playerPosition.x) * (centerX - playerPosition.x) + 
                                                 (centerZ - playerPosition.z) * (centerZ - playerPosition.z));
                    
                    // Add Y distance as secondary priority factor (for sorting only, not filtering)
                    float sectionCenterY = -64.0f + sectionY * 16.0f + 8.0f;
                    float yDistance = std::abs(sectionCenterY - playerPosition.y);
                    
                    // Combined priority: XZ distance is primary, Y distance is secondary
                    // This ensures chunks are processed in square pattern but sections within
                    // a chunk are prioritized by proximity
                    float priority = xzDistance + yDistance * 0.1f;  // Y has less weight
                    
                    sectionCandidates.push_back({chunkPos, sectionY, priority});
                }
            }
        }
        
        // Step 2: Sort sections by priority (closer = higher priority)
        std::sort(sectionCandidates.begin(), sectionCandidates.end(), 
                  [](const auto& a, const auto& b) { return a.priority < b.priority; });
        
        // Step 3: Process sections with budget and queue limits
        const size_t SECTION_SAFETY_CAP = 256;  // Max sections per frame
        size_t sectionsSubmitted = 0;
        auto startTime = std::chrono::steady_clock::now();
        const float budgetMs = 50.0f;  // Time budget for scheduling
        
        // Step 4: Build section snapshots and submit
        for (const auto& candidate : sectionCandidates) {
            // Check safety cap
            if (sectionsSubmitted >= SECTION_SAFETY_CAP) {
                Log::Debug("SCHEDULE: Hit safety cap of %zu sections", SECTION_SAFETY_CAP);
                break;
            }
            
            // Check time budget
            auto currentTime = std::chrono::steady_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(currentTime - startTime).count();
            if (elapsedMs >= budgetMs) {
                Log::Debug("SCHEDULE: Hit time budget of %.1fms", budgetMs);
                break;
            }
            
            // Check queue capacity BEFORE building expensive snapshot
            size_t queueSize = workerPool->GetPendingJobCount();
            const size_t maxQueueSize = workerPool->GetMaxQueueSize();
            if (queueSize >= maxQueueSize * 0.8) {  // Stop at 80% capacity
                Log::Debug("SCHEDULE: Queue nearly full (%zu/%zu), stopping submission", 
                         queueSize, maxQueueSize);
                break;
            }
            
            // Get chunk and section
            auto it = m_chunks.find(candidate.chunkPos);
            if (it == m_chunks.end()) continue;
            
            auto& chunk = it->second;
            auto& sectionInfo = chunk->sectionInfos[candidate.sectionY];
            
            // Double-check section is still dirty and not in flight
            if (!sectionInfo.dirty || sectionInfo.meshingVersion == sectionInfo.version) {
                continue; // Not dirty or already being processed
            }
            
            // Read version optimistically (Minecraft-style)
            const uint32_t expectedVersion = sectionInfo.version;
            
            // Build snapshot (no state checks inside)
            std::shared_ptr<Render::MeshJobData> snapshot;
            if (!BuildSectionSnapshot(candidate.chunkPos, candidate.sectionY, expectedVersion, snapshot)) {
                // Version changed or data missing, retry next frame
                continue;
            }
            
            // Set job type based on section content
            if (sectionInfo.isAllAir) {
                snapshot->jobType = Render::MeshJobType::BorderOnly;
            } else {
                snapshot->jobType = Render::MeshJobType::Full;
            }
            
            // Set priority metadata
            snapshot->distanceToPlayer = candidate.priority;
            snapshot->isHighPriority = (candidate.priority < 128.0f);
            snapshot->submitTime = std::chrono::steady_clock::now();
            
            // Submit section job to worker
            Log::Debug("  Scheduling mesh: chunk(%d,%d) sy=%d ver=%u type=%s neighborMask=0x%X",
                      candidate.chunkPos.x, candidate.chunkPos.z, candidate.sectionY,
                      expectedVersion,
                      sectionInfo.isAllAir ? "BorderOnly" : "Full",
                      snapshot->neighborMask);
            
            // Only update state if submission succeeds (Minecraft-style)
            if (workerPool->SubmitMeshJobWithSnapshot(snapshot)) {
                // Now claim the work after successful submission
                sectionInfo.meshingVersion = expectedVersion;
                sectionInfo.state = SectionState::MESHING;
                sectionInfo.dirty = false;
                chunk->dirtySections.erase(candidate.sectionY);
                sectionsSubmitted++;
            } else {
                // Keep dirty, will retry next frame
                Log::Debug("  Failed to submit mesh job - queue full, will retry");
            }
        }
        
        if (sectionsSubmitted > 0) {
            Log::Debug("SCHEDULE: Submitted %zu section mesh jobs", sectionsSubmitted);
        }
    }
    
    void ClientChunkManager::ScheduleDirtySectionMeshes() {
        // Get player position from somewhere (TODO: implement proper player tracking)
        glm::vec3 playerPos(0, 67, 0);
        ScheduleMeshBuildsWithSnapshots(playerPos);
    }
    
    void ClientChunkManager::CopyNeighborData(Game::Math::ChunkPos chunkPos, int sectionY, 
                                             Render::SectionSnapshot& snapshot) {
        // Copy neighbor chunks (north, south, east, west)
        auto northIt = m_chunks.find({chunkPos.x, chunkPos.z - 1});
        if (northIt != m_chunks.end() && northIt->second && northIt->second->chunkData) {
            auto* neighborSection = northIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    snapshot.neighbors[0][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(snapshot.neighbors[0].begin(), snapshot.neighbors[0].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(snapshot.neighbors[0].begin(), snapshot.neighbors[0].end(), Game::BlockID::Air);
        }
        
        // South (+z)
        auto southIt = m_chunks.find({chunkPos.x, chunkPos.z + 1});
        if (southIt != m_chunks.end() && southIt->second && southIt->second->chunkData) {
            auto* neighborSection = southIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    snapshot.neighbors[1][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(snapshot.neighbors[1].begin(), snapshot.neighbors[1].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(snapshot.neighbors[1].begin(), snapshot.neighbors[1].end(), Game::BlockID::Air);
        }
        
        // East (+x)
        auto eastIt = m_chunks.find({chunkPos.x + 1, chunkPos.z});
        if (eastIt != m_chunks.end() && eastIt->second && eastIt->second->chunkData) {
            auto* neighborSection = eastIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    snapshot.neighbors[2][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(snapshot.neighbors[2].begin(), snapshot.neighbors[2].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(snapshot.neighbors[2].begin(), snapshot.neighbors[2].end(), Game::BlockID::Air);
        }
        
        // West (-x)
        auto westIt = m_chunks.find({chunkPos.x - 1, chunkPos.z});
        if (westIt != m_chunks.end() && westIt->second && westIt->second->chunkData) {
            auto* neighborSection = westIt->second->chunkData->GetSection(sectionY);
            if (neighborSection) {
                for (int i = 0; i < 4096; ++i) {
                    snapshot.neighbors[3][i] = static_cast<Game::BlockID>(neighborSection->blocks[i]);
                }
            } else {
                std::fill(snapshot.neighbors[3].begin(), snapshot.neighbors[3].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(snapshot.neighbors[3].begin(), snapshot.neighbors[3].end(), Game::BlockID::Air);
        }
        
        // Up (same chunk, section above)
        auto& chunk = m_chunks[chunkPos];
        if (sectionY < 23) {
            auto* upSection = chunk->chunkData->GetSection(sectionY + 1);
            if (upSection) {
                for (int i = 0; i < 4096; ++i) {
                    snapshot.neighbors[4][i] = static_cast<Game::BlockID>(upSection->blocks[i]);
                }
            } else {
                std::fill(snapshot.neighbors[4].begin(), snapshot.neighbors[4].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(snapshot.neighbors[4].begin(), snapshot.neighbors[4].end(), Game::BlockID::Air);
        }
        
        // Down (same chunk, section below)
        if (sectionY > 0) {
            auto* downSection = chunk->chunkData->GetSection(sectionY - 1);
            if (downSection) {
                for (int i = 0; i < 4096; ++i) {
                    snapshot.neighbors[5][i] = static_cast<Game::BlockID>(downSection->blocks[i]);
                }
            } else {
                std::fill(snapshot.neighbors[5].begin(), snapshot.neighbors[5].end(), Game::BlockID::Air);
            }
        } else {
            std::fill(snapshot.neighbors[5].begin(), snapshot.neighbors[5].end(), Game::BlockID::Air);
        }
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
        
        // Check if this result is stale (version mismatch)
        if (result.generation != sectionInfo.meshingVersion) {
            // Stale result - section was modified while meshing
            // Log::Debug("[mesh] drop STALE cx=%d cz=%d sy=%d resVer=%u curVer=%u",
            //           result.chunkPos.x, result.chunkPos.z, result.sectionY,
            //           result.generation, sectionInfo.meshingVersion);
            
            // Reset meshingVersion to allow rescheduling with new version
            sectionInfo.meshingVersion = 0;
            // Explicitly set dirty flag to ensure reschedule (Minecraft-style)
            sectionInfo.dirty = true;
            chunk->dirtySections.insert(result.sectionY);
            
            return { MeshApplyAction::Drop_StaleVersion };
        }
        
        // Check if section was modified after mesh was scheduled
        if (sectionInfo.meshingVersion != sectionInfo.version) {
            // Section changed while mesh was in flight (e.g., another neighbor loaded)
            Log::Debug("[mesh] DROP STALE: chunk(%d,%d) sy=%d meshVer=%u curVer=%u neighborMask=0x%X (modified after scheduling)",
                      result.chunkPos.x, result.chunkPos.z, result.sectionY,
                      sectionInfo.meshingVersion, sectionInfo.version, result.neighborMask);
            
            // Reset meshingVersion to allow rescheduling with new version
            sectionInfo.meshingVersion = 0;
            // Explicitly set dirty flag to ensure reschedule (Minecraft-style)
            sectionInfo.dirty = true;
            chunk->dirtySections.insert(result.sectionY);
            
            return { MeshApplyAction::Drop_StaleVersion };
        }
        
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
        
        // Double-check version hasn't changed (extremely rare race)
        if (sectionInfo.version != sectionInfo.meshingVersion) {
            Log::Warning("Version changed during upload for chunk (%d, %d) section %d",
                        chunkPos.x, chunkPos.z, sectionY);
            return;
        }
        
        // Mark section as ready and clear dirty flag
        sectionInfo.state = SectionState::READY;
        sectionInfo.dirty = false;
        sectionInfo.builtOnce = true;  // Mark as built at least once
        
        // Clear from legacy dirty set if present
        chunk->dirtySections.erase(sectionY);
        
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

} // namespace Client