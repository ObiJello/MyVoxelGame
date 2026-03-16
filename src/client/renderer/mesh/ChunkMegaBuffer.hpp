// File: src/client/renderer/mesh/ChunkMegaBuffer.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "../backend/RenderTypes.hpp"
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstddef>

namespace Render {

    // Section identifier for mega-buffer regions (matches ClientMeshManager::SectionKey layout)
    struct MegaBufferSectionKey {
        Game::Math::ChunkPos chunkPos;
        int sectionY;

        bool operator==(const MegaBufferSectionKey& other) const {
            return chunkPos.x == other.chunkPos.x &&
                   chunkPos.z == other.chunkPos.z &&
                   sectionY == other.sectionY;
        }
    };

    struct MegaBufferSectionKeyHash {
        std::size_t operator()(const MegaBufferSectionKey& key) const {
            size_t h = std::hash<int32_t>{}(key.chunkPos.x);
            h ^= std::hash<int32_t>{}(key.chunkPos.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(key.sectionY) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // ========================================================================
    // GPU MEGA-BUFFER (Slab Pool Architecture)
    // ========================================================================
    //
    // Packs chunk section vertices/indices for one render layer into a pool of
    // fixed-size GPU buffer slabs, enabling multi-draw rendering via
    // the render backend without buffer grow hitches.
    //
    // One ChunkMegaBuffer is used per render layer (opaque, cutout, translucent).
    // When a slab fills up, a new empty slab is allocated (<1ms, no data copy).
    // Sections are uploaded to whichever slab has free space.
    //
    // Free-list allocator per slab manages regions with first-fit and coalescing.
    //
    class ChunkMegaBuffer {
    public:
        ChunkMegaBuffer() = default;
        ~ChunkMegaBuffer();

        // Non-copyable
        ChunkMegaBuffer(const ChunkMegaBuffer&) = delete;
        ChunkMegaBuffer& operator=(const ChunkMegaBuffer&) = delete;

        // ========================================================================
        // LIFECYCLE
        // ========================================================================

        // Initialize with a fixed slab size. The first slab is allocated immediately.
        void Initialize(size_t slabVertexCapacity = 512000, size_t slabIndexCapacity = 1024000);
        void Shutdown();
        bool IsInitialized() const { return !m_slabs.empty(); }

        // ========================================================================
        // SECTION MANAGEMENT
        // ========================================================================

        bool UploadSection(const MegaBufferSectionKey& key,
                           const float* vertexData, size_t vertexCount,
                           const uint32_t* indexData, size_t indexCount);

        void RemoveSection(const MegaBufferSectionKey& key);
        bool HasSection(const MegaBufferSectionKey& key) const;

        // ========================================================================
        // DRAW COMMANDS
        // ========================================================================

        struct DrawCommand {
            int32_t indexCount;
            size_t indexByteOffset;   // Byte offset into slab's IBO
            int32_t baseVertex;       // Added to each index by the backend
            uint32_t slabIndex;       // Which slab to bind before drawing
        };

        bool GetDrawCommand(const MegaBufferSectionKey& key, DrawCommand& outCmd) const;

        // ========================================================================
        // SLAB BINDING
        // ========================================================================

        // Bind a specific slab's VBO and IBO via the render backend.
        void BindSlab(uint32_t slabIndex) const;

        uint32_t GetSlabCount() const { return static_cast<uint32_t>(m_slabs.size()); }

        // ========================================================================
        // STATISTICS
        // ========================================================================

        size_t GetSectionCount() const { return m_regions.size(); }
        size_t GetMemoryUsageBytes() const;
        size_t GetTotalVertexCapacity() const;
        size_t GetTotalIndexCapacity() const;
        size_t GetUsedVertices() const;
        size_t GetUsedIndices() const;

        // ========================================================================
        // MAINTENANCE
        // ========================================================================

        // No-copy cleanup: just deletes completely empty slabs.
        // Returns true if any slabs were removed.
        bool CompactIfNeeded(float threshold = 0.5f);

    private:
        // Per-slab GPU resources and allocator state
        struct Slab {
            BufferHandle vbo = INVALID_BUFFER;
            BufferHandle ibo = INVALID_BUFFER;
            size_t vboCapacity = 0;
            size_t iboCapacity = 0;
            size_t vertexHighWater = 0;
            size_t indexHighWater = 0;
            size_t sectionCount = 0;  // Live sections in this slab

            // Free-list per slab (sorted by offset for coalescing)
            struct FreeBlock {
                size_t offset;
                size_t size;
            };
            std::vector<FreeBlock> freeVertexBlocks;
            std::vector<FreeBlock> freeIndexBlocks;
        };

        std::vector<Slab> m_slabs;
        size_t m_slabVertexCapacity = 0;
        size_t m_slabIndexCapacity = 0;

        // Per-section region tracking (which slab + offset)
        struct Region {
            uint32_t slabIndex;
            size_t vertexOffset;
            size_t vertexCount;
            size_t indexOffset;
            size_t indexCount;
        };
        std::unordered_map<MegaBufferSectionKey, Region, MegaBufferSectionKeyHash> m_regions;

        // Slab management
        uint32_t AllocateSlab();
        bool TryUploadToSlab(uint32_t slabIndex, const MegaBufferSectionKey& key,
                             const float* vertexData, size_t vertexCount,
                             const uint32_t* indexData, size_t indexCount);

        // Internal allocation (first-fit with bump fallback, per-slab)
        static bool AllocRegion(std::vector<Slab::FreeBlock>& freeList, size_t& highWater,
                                size_t capacity, size_t count, size_t& outOffset);
        static void FreeRegion(std::vector<Slab::FreeBlock>& freeList,
                               size_t offset, size_t count);

        static constexpr size_t VERTEX_STRIDE = 24;
        static constexpr size_t INDEX_SIZE = sizeof(uint32_t);
    };

} // namespace Render
