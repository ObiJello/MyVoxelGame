// File: src/client/renderer/mesh/MeshJobData.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include <array>
#include <memory>
#include <chrono>

namespace Client {
namespace Render {

    // Snapshot of section data for thread-safe mesh building
    // This is a COPY of the chunk data that workers can safely read
    struct SectionSnapshot {
        // Block data (16x16x16 = 4096 blocks)
        std::array<Game::BlockID, 4096> blocks;
        
        // Light data (sky and block light, 4 bits each)
        std::array<uint8_t, 2048> lightData;
        
        // Neighbor section snapshots for face culling
        // Index: 0=north, 1=south, 2=east, 3=west, 4=up, 5=down
        std::array<std::array<Game::BlockID, 4096>, 6> neighbors;
        
        // Metadata
        bool isEmpty = true;
        int sectionY = 0;
        
        // Copy block at local coordinates (0-15)
        void SetBlock(int x, int y, int z, Game::BlockID block) {
            if (x >= 0 && x < 16 && y >= 0 && y < 16 && z >= 0 && z < 16) {
                blocks[y * 256 + z * 16 + x] = block;
                if (block != Game::BlockID::Air) {
                    isEmpty = false;
                }
            }
        }
        
        // Get block at local coordinates (0-15)
        Game::BlockID GetBlock(int x, int y, int z) const {
            if (x >= 0 && x < 16 && y >= 0 && y < 16 && z >= 0 && z < 16) {
                return blocks[y * 256 + z * 16 + x];
            }
            return Game::BlockID::Air;
        }
        
        // Get block from neighbor section
        Game::BlockID GetNeighborBlock(int face, int x, int y, int z) const {
            if (face >= 0 && face < 6 && x >= 0 && x < 16 && 
                y >= 0 && y < 16 && z >= 0 && z < 16) {
                return neighbors[face][y * 256 + z * 16 + x];
            }
            return Game::BlockID::Air;
        }
    };

    // Job type for mesh processing
    enum class MeshJobType {
        Full,        // Normal meshing for non-empty sections
        BorderOnly   // Fast path for empty sections - only compute neighbor mask
    };
    
    // Complete mesh job data with all information needed for meshing
    struct MeshJobData {
        // Chunk position
        Game::Math::ChunkPos chunkPos;
        
        // Section Y coordinate (-4 to 19 for world height -64 to 319)
        int sectionY;
        
        // Snapshot of this section's data
        SectionSnapshot sectionData;
        
        // Job type (full mesh or border-only for empty sections)
        MeshJobType jobType = MeshJobType::Full;
        
        // Priority information
        bool isHighPriority = false;
        float distanceToPlayer = 0.0f;
        
        // Timing
        std::chrono::steady_clock::time_point submitTime;
        
        // Generation ID for staleness checking
        uint32_t generation = 0;
        
        // Neighbor chunk presence mask (PX=1, NX=2, PZ=4, NZ=8)
        // Computed on main thread where we know which chunks exist
        uint8_t neighborMask = 0;
        
        MeshJobData() : submitTime(std::chrono::steady_clock::now()) {}
        
        MeshJobData(Game::Math::ChunkPos pos, int secY) 
            : chunkPos(pos)
            , sectionY(secY)
            , submitTime(std::chrono::steady_clock::now()) {}
    };

    // Result of mesh building on worker thread
    struct MeshResult {
        // Position information
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        
        // Generation ID to check if result is still valid
        uint32_t generation;
        
        // Mesh data by render layer
        struct LayerData {
            std::vector<float> vertices;
            std::vector<uint32_t> indices;
            size_t vertexCount = 0;
            size_t indexCount = 0;
            
            bool IsEmpty() const { return vertices.empty(); }
            
            size_t GetMemorySize() const {
                return vertices.size() * sizeof(float) + 
                       indices.size() * sizeof(uint32_t);
            }
        };
        
        // Three render layers (following Minecraft's rendering order)
        LayerData opaqueLayer;      // Solid blocks (stone, dirt, etc.)
        LayerData cutoutLayer;      // Alpha-tested blocks (leaves, grass)
        LayerData translucentLayer; // Blended blocks (water, glass, ice)
        
        // Neighbor presence mask computed during meshing (PX=1, NX=2, PZ=4, NZ=8)
        uint8_t neighborMask = 0;
        
        // =====================================================================
        // TRANSLUCENCY REBUILD POLICY
        // =====================================================================
        // This implementation follows Minecraft's approach: translucent sections
        // are REBUILT when any block or light change occurs, rather than
        // re-sorted per frame. This is simpler and more stable.
        //
        // Policy details:
        // 1. Translucent geometry is sorted ONCE during mesh build (back-to-front)
        // 2. Any block change in section → full section rebuild (all layers)
        // 3. Light-only changes → rebuild only if section has translucent blocks
        // 4. No per-frame sorting of terrain geometry (too expensive)
        // 5. Translucent entities are sorted separately per frame (not terrain)
        //
        // Rationale:
        // - Avoids expensive per-frame sorting of chunk geometry
        // - Translucent blocks in terrain rarely change
        // - Most translucency issues are "good enough" with static sorting
        // - Matches Minecraft Java Edition's behavior
        // =====================================================================
        
        // Build statistics
        bool success = false;
        std::chrono::steady_clock::time_point completeTime;
        std::chrono::milliseconds buildDuration;
        
        // Check if any layer has geometry
        bool IsEmpty() const {
            return opaqueLayer.IsEmpty() && 
                   cutoutLayer.IsEmpty() && 
                   translucentLayer.IsEmpty();
        }
        
        // Get total memory size
        size_t GetTotalMemorySize() const {
            return opaqueLayer.GetMemorySize() + 
                   cutoutLayer.GetMemorySize() + 
                   translucentLayer.GetMemorySize();
        }
        
        MeshResult() : completeTime(std::chrono::steady_clock::now()) {}
        
        MeshResult(Game::Math::ChunkPos pos, int secY, uint32_t gen)
            : chunkPos(pos)
            , sectionY(secY)
            , generation(gen)
            , completeTime(std::chrono::steady_clock::now()) {}
    };

    // Priority comparator for mesh job queue
    struct MeshJobPriority {
        bool operator()(const std::shared_ptr<MeshJobData>& a, 
                       const std::shared_ptr<MeshJobData>& b) const {
            // High priority jobs always come first
            if (a->isHighPriority != b->isHighPriority) {
                return !a->isHighPriority; // Priority queue is max-heap, so invert
            }
            
            // Otherwise sort by distance (closer = higher priority)
            return a->distanceToPlayer > b->distanceToPlayer;
        }
    };

} // namespace Render
} // namespace Client