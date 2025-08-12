// File: src/client/renderer/mesh/SnapshotBuilder.hpp
#pragma once

#include "MeshJobData.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/core/Log.hpp"

namespace Client {
namespace Render {

    // Builds thread-safe snapshots of chunk sections for mesh generation
    // This runs on the MAIN THREAD and creates copies that workers can safely read
    class SnapshotBuilder {
    public:
        // Create a snapshot of a chunk section for meshing
        // This COPIES all necessary data so workers don't access live chunks
        static std::shared_ptr<MeshJobData> CreateSnapshot(
            const Client::ClientChunk* chunk,
            int sectionY,
            const Client::ClientChunkManager* chunkManager,
            const glm::vec3& playerPos) {
            
            if (!chunk || !chunk->chunkData || !chunk->IsLoaded()) {
                return nullptr;
            }
            
            auto snapshot = std::make_shared<MeshJobData>(chunk->position, sectionY);
            snapshot->generation = chunk->generation;
            
            // Get the section
            const auto* section = chunk->chunkData->GetSection(sectionY);
            if (!section) {
                // Empty section
                snapshot->sectionData.isEmpty = true;
                return snapshot;
            }
            
            // Copy block data from section
            CopySectionBlocks(section, snapshot->sectionData);
            snapshot->sectionData.sectionY = sectionY;  // Set the section Y coordinate
            
            // Copy neighbor sections for face culling
            CopyNeighborSections(chunk->position, sectionY, chunkManager, snapshot->sectionData);
            
            // Calculate priority
            CalculatePriority(chunk->position, sectionY, playerPos, *snapshot);
            
            return snapshot;
        }
        
    private:
        // Copy blocks from a section to snapshot
        static void CopySectionBlocks(const Game::ChunkSection* section, SectionSnapshot& snapshot) {
            snapshot.isEmpty = true;
            // sectionY is passed separately to CreateSnapshot and set there
            
            // Copy all blocks
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        int index = y * 256 + z * 16 + x;
                        // Explicit cast from uint16_t to BlockID enum
                        Game::BlockID block = static_cast<Game::BlockID>(section->blocks[index]);
                        snapshot.blocks[index] = block;
                        
                        if (block != Game::BlockID::Air) {
                            snapshot.isEmpty = false;
                        }
                    }
                }
            }
            
            // TODO: Copy light data when implemented
            std::fill(snapshot.lightData.begin(), snapshot.lightData.end(), 0xFF);
        }
        
        // Copy neighbor sections for face culling
        static void CopyNeighborSections(
            Game::Math::ChunkPos chunkPos,
            int sectionY,
            const Client::ClientChunkManager* chunkManager,
            SectionSnapshot& snapshot) {
            
            // Face indices: 0=north(-z), 1=south(+z), 2=east(+x), 3=west(-x), 4=up(+y), 5=down(-y)
            
            // Horizontal neighbors (different chunks)
            CopyNeighborChunkSection(chunkPos, sectionY, {0, -1}, chunkManager, snapshot.neighbors[0]); // North
            CopyNeighborChunkSection(chunkPos, sectionY, {0, 1}, chunkManager, snapshot.neighbors[1]);  // South
            CopyNeighborChunkSection(chunkPos, sectionY, {1, 0}, chunkManager, snapshot.neighbors[2]);  // East
            CopyNeighborChunkSection(chunkPos, sectionY, {-1, 0}, chunkManager, snapshot.neighbors[3]); // West
            
            // Vertical neighbors (same chunk, different sections)
            const auto* chunk = chunkManager->GetChunk(chunkPos);
            if (chunk && chunk->chunkData) {
                // Up (section above)
                if (sectionY < Game::Math::SECTIONS_PER_CHUNK - 1) {
                    const auto* upSection = chunk->chunkData->GetSection(sectionY + 1);
                    if (upSection) {
                        // Manual copy with explicit cast
                        for (size_t i = 0; i < 4096; ++i) {
                            snapshot.neighbors[4][i] = static_cast<Game::BlockID>(upSection->blocks[i]);
                        }
                    } else {
                        std::fill(snapshot.neighbors[4].begin(), snapshot.neighbors[4].end(), 
                                 Game::BlockID::Air);
                    }
                } else {
                    std::fill(snapshot.neighbors[4].begin(), snapshot.neighbors[4].end(), 
                             Game::BlockID::Air);
                }
                
                // Down (section below)
                if (sectionY > 0) {
                    const auto* downSection = chunk->chunkData->GetSection(sectionY - 1);
                    if (downSection) {
                        // Manual copy with explicit cast
                        for (size_t i = 0; i < 4096; ++i) {
                            snapshot.neighbors[5][i] = static_cast<Game::BlockID>(downSection->blocks[i]);
                        }
                    } else {
                        std::fill(snapshot.neighbors[5].begin(), snapshot.neighbors[5].end(), 
                                 Game::BlockID::Air);
                    }
                } else {
                    std::fill(snapshot.neighbors[5].begin(), snapshot.neighbors[5].end(), 
                             Game::BlockID::Air);
                }
            } else {
                // No chunk data - fill with air
                std::fill(snapshot.neighbors[4].begin(), snapshot.neighbors[4].end(), Game::BlockID::Air);
                std::fill(snapshot.neighbors[5].begin(), snapshot.neighbors[5].end(), Game::BlockID::Air);
            }
        }
        
        // Copy a section from a neighbor chunk
        static void CopyNeighborChunkSection(
            Game::Math::ChunkPos chunkPos,
            int sectionY,
            std::pair<int, int> offset,
            const Client::ClientChunkManager* chunkManager,
            std::array<Game::BlockID, 4096>& neighborBlocks) {
            
            Game::Math::ChunkPos neighborPos{chunkPos.x + offset.first, chunkPos.z + offset.second};
            const auto* neighborChunk = chunkManager->GetChunk(neighborPos);
            
            if (neighborChunk && neighborChunk->chunkData && neighborChunk->IsLoaded()) {
                const auto* section = neighborChunk->chunkData->GetSection(sectionY);
                if (section) {
                    // Manual copy with explicit cast
                    for (size_t i = 0; i < 4096; ++i) {
                        neighborBlocks[i] = static_cast<Game::BlockID>(section->blocks[i]);
                    }
                } else {
                    std::fill(neighborBlocks.begin(), neighborBlocks.end(), Game::BlockID::Air);
                }
            } else {
                // Neighbor chunk not loaded - fill with air
                std::fill(neighborBlocks.begin(), neighborBlocks.end(), Game::BlockID::Air);
            }
        }
        
        // Calculate priority based on distance to player
        static void CalculatePriority(
            Game::Math::ChunkPos chunkPos,
            int sectionY,
            const glm::vec3& playerPos,
            MeshJobData& job) {
            
            // Calculate center of section in world space
            float centerX = chunkPos.x * 16.0f + 8.0f;
            float centerY = sectionY * 16.0f - 64.0f + 8.0f; // Account for world offset
            float centerZ = chunkPos.z * 16.0f + 8.0f;
            
            glm::vec3 sectionCenter(centerX, centerY, centerZ);
            job.distanceToPlayer = glm::distance(playerPos, sectionCenter);
            
            // Mark as high priority if within 3 chunks (48 blocks)
            job.isHighPriority = (job.distanceToPlayer < 48.0f);
        }
    };

} // namespace Render
} // namespace Client