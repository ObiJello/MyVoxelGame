// File: src/client/renderer/mesh/MeshJobData.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/chunk/PalettedContainer.hpp"
#include "common/world/block/BlockStateIds.hpp"
#include <array>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>

namespace Client {
namespace Render {

    // ── Shared per-section copy (MC SectionCopy) ────────────────────────
    //
    // One immutable copy of ONE section, shared by every mesh job that needs
    // it. Direct port of net.minecraft.client.renderer.chunk.SectionCopy:
    // taken on the main thread, handed to workers by shared_ptr, never mutated.
    //
    // Sharing is the whole point. A section is read by its own mesh job AND by
    // the 26 jobs around it, so a private copy per job duplicates the same
    // blocks 27 times. MC keeps one copy per section in a per-frame cache
    // (RenderRegionCache) and hands out references; this does the same.
    //
    // MC sets `section = null` for a section that hasOnlyAir() and answers AIR
    // for every read. `allAir` is that: most of a 384-block column is sky, and
    // those sections cost a control block and nothing else.
    struct SectionCopy {
        bool allAir = true;

        // MC SectionCopy keeps `levelChunkSection.getStates().copy()` — the
        // section's own PalettedContainer, not an unpacked array. So does this
        // now: a copy is the palette plus the packed words, roughly 2 KB for
        // ordinary terrain against the 8 KB flat array it replaced, and nothing
        // at all for a uniform section.
        //
        // That matters more here than anywhere else: the compile queue is
        // unbounded, so a pass holds one copy per distinct section it touched.
        Game::PalettedContainer states;

        // Biomes: 4x4x4 quart cells for this section.
        //
        // MC does NOT carry these — RenderSectionRegion.getBlockTint delegates
        // to the live Level from the worker thread. We deliberately do not: our
        // mesh workers must never touch the live chunk map, and MC gets away
        // with it only because its ClientLevel chunk array is safe to read
        // concurrently.
        bool hasBiomes = false;
        Game::PalettedContainer biomes;

        Game::BlockID GetBlock(int lx, int ly, int lz) const {
            if (allAir) return Game::BlockID::Air;
            return Game::BlockStateIds::Unpack(
                       states.Get(static_cast<size_t>(Game::Math::LocalIndex(lx, ly, lz)))).id;
        }

        uint8_t GetState(int lx, int ly, int lz) const {
            if (allAir) return 0;
            return Game::BlockStateIds::Unpack(
                       states.Get(static_cast<size_t>(Game::Math::LocalIndex(lx, ly, lz)))).state;
        }

        // Flat state id without unpacking — the mesher's cache fill wants the
        // block and the state together, so it pays one Unpack instead of two.
        uint32_t GetStateId(size_t index) const {
            return allAir ? Game::BlockStateIds::Pack(Game::BlockID::Air, 0) : states.Get(index);
        }

        uint16_t GetBiome(int qx, int qy, int qz) const {
            if (!hasBiomes) return Game::kFallbackBiomeId;
            return static_cast<uint16_t>(
                biomes.Get(static_cast<size_t>((qy * 4 + qz) * 4 + qx)));
        }
    };

    using SectionCopyPtr = std::shared_ptr<const SectionCopy>;

    // ── The 3x3x3 neighbourhood a section is meshed against ─────────────────
    //
    // Port of MC RenderSectionRegion (RADIUS 1, SIZE 3, 27 sections). Holds
    // REFERENCES, so constructing one is 27 pointer copies.
    //
    // Carrying all 27 is also a correctness fix, not only a memory one: the
    // previous snapshot carried the six face planes and synthesised edges and
    // corners with a dominant-axis clamp, which is an approximation that shows
    // up as wrong ambient occlusion along section borders. Every neighbour is
    // now present, so those reads are exact.
    struct RegionSnapshot {
        static constexpr int RADIUS = 1;
        static constexpr int SIZE   = 3;

        // Section coordinates of the corner (centre - 1 on each axis).
        int minSectionX = 0, minSectionY = 0, minSectionZ = 0;
        std::array<SectionCopyPtr, SIZE * SIZE * SIZE> sections;

        static int Index(int dx, int dy, int dz) {
            return (dz * SIZE + dy) * SIZE + dx;   // dx,dy,dz in [0,2]
        }

        // `lx/ly/lz` are LOCAL to the centre section and may run from -16 to 31;
        // meshing only ever reaches -1..16.
        const SectionCopy* SectionForLocal(int lx, int ly, int lz) const {
            const int dx = (lx >> 4) + RADIUS;
            const int dy = (ly >> 4) + RADIUS;
            const int dz = (lz >> 4) + RADIUS;
            if (dx < 0 || dx >= SIZE || dy < 0 || dy >= SIZE || dz < 0 || dz >= SIZE) {
                return nullptr;
            }
            return sections[Index(dx, dy, dz)].get();
        }

        Game::BlockID BlockAtLocal(int lx, int ly, int lz) const {
            const SectionCopy* sec = SectionForLocal(lx, ly, lz);
            if (!sec) return Game::BlockID::Air;
            return sec->GetBlock(lx & 15, ly & 15, lz & 15);
        }

        // Flat state id, for the cache fill that wants block and state together.
        uint32_t StateIdAtLocal(int lx, int ly, int lz) const {
            const SectionCopy* sec = SectionForLocal(lx, ly, lz);
            if (!sec) return Game::BlockStateIds::Pack(Game::BlockID::Air, 0);
            return sec->GetStateId(
                static_cast<size_t>(Game::Math::LocalIndex(lx & 15, ly & 15, lz & 15)));
        }

        uint16_t BiomeAtLocal(int lx, int ly, int lz) const {
            const SectionCopy* sec = SectionForLocal(lx, ly, lz);
            if (!sec) return Game::kFallbackBiomeId;
            // Block-local -> quart cell within the section.
            return sec->GetBiome((lx & 15) >> 2, (ly & 15) >> 2, (lz & 15) >> 2);
        }

        const SectionCopy* Centre() const {
            return sections[Index(RADIUS, RADIUS, RADIUS)].get();
        }

        bool CentreIsEmpty() const {
            const SectionCopy* c = Centre();
            return !c || c->allAir;
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
        
        // The 3x3x3 neighbourhood this section is meshed against. Shares its
        // SectionCopy objects with every other job built in the same pass.
        RegionSnapshot region;
        
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