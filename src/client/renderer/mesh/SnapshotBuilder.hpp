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
        
        // Legacy neighbor copy — this code path is unused (BuildSectionSnapshot in
        // ClientChunkManager.cpp handles snapshot creation with boundary-only planes).
        // Kept as stubs for compilation.
        static void CopyNeighborSections(
            Game::Math::ChunkPos, int, const Client::ClientChunkManager*, SectionSnapshot& snapshot) {
            for (auto& plane : snapshot.neighbors)
                std::fill(plane.begin(), plane.end(), Game::BlockID::Air);
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