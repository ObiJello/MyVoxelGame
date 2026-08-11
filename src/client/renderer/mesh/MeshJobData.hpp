// File: src/client/renderer/mesh/MeshJobData.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/biome/Biomes.hpp"
#include <array>
#include <vector>
#include <atomic>
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
        
        // Neighbor boundary planes for face culling (16x16 = 256 blocks per face).
        // Only the single layer adjacent to this section is needed (mesher reads 1 block into neighbors).
        // Index: 0=north(-z, stores z=15), 1=south(+z, stores z=0),
        //        2=east(+x, stores x=0), 3=west(-x, stores x=15),
        //        4=up(+y, stores y=0), 5=down(-y, stores y=15)
        std::array<std::array<Game::BlockID, 256>, 6> neighbors;
        
        // Per-voxel block-state indices, in the same [y*256 + z*16 + x] layout
        // as `blocks`. Left EMPTY when the source section carries no states,
        // which is the case for essentially all terrain (ChunkSection::states
        // is lazily allocated for the same reason) — an always-present array
        // would add 4 KB to every snapshot in flight for nothing.
        //
        // No neighbour planes: unlike block ids, states are only read for the
        // block being meshed. Face culling and AO care about the neighbour's
        // opacity, which is a property of the block id alone.
        std::vector<uint8_t> states;

        // Noise biomes covering this section PLUS the margin MC's biome blend
        // reaches. ClientLevel.calculateBlockTint averages a
        // (2*biomeBlendRadius+1)^2 square with the vanilla default radius of 2,
        // so a tint at x needs biomes from x-2 to x+17 — quart cells -1 through
        // 4, i.e. 6 per horizontal axis. Vertically only the section's own four
        // quart layers are needed, because the blend samples at a fixed Y.
        //
        // 6*4*6 = 144 entries (288 bytes), always present when the chunk has
        // biome data. Copying it into the snapshot is what keeps mesh workers
        // off the live chunk map.
        static constexpr int BIOME_XZ = 6;
        static constexpr int BIOME_Y  = 4;
        std::array<uint16_t, BIOME_XZ * BIOME_Y * BIOME_XZ> biomes{};
        bool hasBiomes = false;

        // `lx`/`lz` are section-local BLOCK coordinates and may run from -2 to
        // 17; `ly` is 0..15. Out-of-grid asks clamp, which only happens if a
        // caller reaches further than the blend radius.
        uint16_t GetBiomeLocal(int lx, int ly, int lz) const {
            if (!hasBiomes) return Game::kFallbackBiomeId;
            auto quart = [](int v) {
                // Floor-divide by 4 then shift into the grid (which starts at
                // quart cell -1). Arithmetic shift keeps negatives flooring.
                const int q = (v >> 2) + 1;
                return q < 0 ? 0 : (q >= BIOME_XZ ? BIOME_XZ - 1 : q);
            };
            const int qx = quart(lx);
            const int qz = quart(lz);
            int qy = ly >> 2;
            if (qy < 0) qy = 0; else if (qy >= BIOME_Y) qy = BIOME_Y - 1;
            return biomes[static_cast<size_t>((qy * BIOME_XZ + qz) * BIOME_XZ + qx)];
        }

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

        void SetBlockState(int x, int y, int z, uint8_t stateIndex) {
            if (x < 0 || x >= 16 || y < 0 || y >= 16 || z < 0 || z >= 16) return;
            if (states.empty()) {
                if (stateIndex == 0) return;   // still all-default; stay unallocated
                states.assign(4096, 0);
            }
            states[y * 256 + z * 16 + x] = stateIndex;
        }

        uint8_t GetBlockState(int x, int y, int z) const {
            if (states.empty()) return 0;
            if (x < 0 || x >= 16 || y < 0 || y >= 16 || z < 0 || z >= 16) return 0;
            return states[y * 256 + z * 16 + x];
        }
        
        // Get block at local coordinates (0-15)
        Game::BlockID GetBlock(int x, int y, int z) const {
            if (x >= 0 && x < 16 && y >= 0 && y < 16 && z >= 0 && z < 16) {
                return blocks[y * 256 + z * 16 + x];
            }
            return Game::BlockID::Air;
        }
        
        // Get block from neighbor boundary plane.
        // Each face stores a 16x16 plane (256 blocks) — the single layer adjacent to this section.
        // Coordinates are remapped to 2D based on which axis the face is perpendicular to.
        Game::BlockID GetNeighborBlock(int face, int x, int y, int z) const {
            if (face < 0 || face >= 6) return Game::BlockID::Air;
            int idx;
            switch (face) {
                case 0: case 1: idx = y * 16 + x; break;  // N/S: plane perpendicular to Z
                case 2: case 3: idx = y * 16 + z; break;  // E/W: plane perpendicular to X
                case 4: case 5: idx = z * 16 + x; break;  // U/D: plane perpendicular to Y
                default: return Game::BlockID::Air;
            }
            if (idx < 0 || idx >= 256) return Game::BlockID::Air;
            return neighbors[face][idx];
        }
    };

    // Job type for mesh processing
    enum class MeshJobType {
        Initial,     // First compile for a newly loaded section (highest priority)
        Full,        // Recompile after block change
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

        // Per-task cancellation flag (Minecraft-style AtomicBoolean isCancelled)
        std::atomic<bool> cancelled{false};
        void Cancel() { cancelled.store(true, std::memory_order_release); }
        bool IsCancelled() const { return cancelled.load(std::memory_order_acquire); }

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