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
        // perSectionIndexBuffers: give every section its own index buffer instead
        // of a range inside the shared slab IBO. This is what MC does
        // (CompiledSectionMesh.uploadLayerIndexBuffer writes to a per-section,
        // per-layer buffer), and it exists for the TRANSLUCENT pool specifically:
        // a translucency re-sort rewrites indices every few frames, and writing
        // into a shared multi-megabyte IBO that has draws in flight makes the GL
        // driver serialise. Measured at 0.18 ms per write, 16x the cost of the
        // sort itself. Vertices still live in the shared slab, so baseVertex
        // batching is unaffected — only the index buffer is split out.
        //
        // Cost: the layer can no longer be issued as one multi-draw per slab; it
        // becomes one bind + draw per section. Worth it only where re-sorts are
        // frequent, i.e. translucent. Leave false for opaque/cutout.
        void Initialize(size_t slabVertexCapacity = 512000, size_t slabIndexCapacity = 1024000,
                        bool perSectionIndexBuffers = false);

        // Per-section IBO for this section, or INVALID_BUFFER when the pool uses
        // shared slab indices. The renderer binds this before drawing the section.
        BufferHandle GetSectionIndexBuffer(const MegaBufferSectionKey& key) const;

        bool UsesPerSectionIndexBuffers() const { return m_perSectionIndexBuffers; }

        // Hand back slab ranges freed a few frames ago. MUST be called once per
        // frame — without it RemoveSection leaks the range permanently.
        //
        // Why the delay exists, because it is a correctness fix and not a
        // tuning knob: RemoveSection used to return a range to the free-list
        // immediately, and re-meshing a section (breaking a block) is a
        // RemoveSection followed instantly by an UploadSection that allocates
        // the SAME range straight back and memcpys new geometry into it. The
        // GPU is still reading that range for the previous frame, which is
        // in-flight — uploads run before BeginFrame's fence wait. On OpenGL the
        // driver hides this because glBufferSubData serialises against pending
        // draws; on Vulkan the write lands in HOST_VISIBLE memory with nothing
        // to order it, so the GPU renders a mix of the old and new mesh for one
        // frame. Symptom: breaking a block occasionally flashes a hole through
        // the world for a single frame, on Vulkan only.
        //
        // Delaying reuse means a re-upload always lands on memory nothing is
        // reading, and the range the GPU IS reading is never written.
        void RetireFreedRegions();
        void Shutdown();
        bool IsInitialized() const { return !m_slabs.empty(); }

        // ========================================================================
        // SECTION MANAGEMENT
        // ========================================================================

        bool UploadSection(const MegaBufferSectionKey& key,
                           const float* vertexData, size_t vertexCount,
                           const uint16_t* indexData, size_t indexCount);

        // Rewrites a section's indices in place, leaving its vertices alone.
        // Used by translucent re-sorting, which reorders quads without
        // regenerating any geometry. `indexCount` must equal what the section
        // was uploaded with — the region is not reallocated — otherwise the
        // call is rejected and the old order stays on the GPU.
        bool UpdateSectionIndices(const MegaBufferSectionKey& key,
                                  const uint16_t* indexData, size_t indexCount);

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

        // Bytes handed to the driver since the last call, then reset. This is
        // the ONLY number that tracks what the GPU actually has to move —
        // every write to a slab (mesh upload and translucency re-sort alike)
        // funnels through UploadSection/UpdateSectionIndices. CPU time spent
        // issuing those writes does not track it, because the driver stages
        // the copy and returns; the transfer is paid at the swap.
        size_t ConsumeUploadedBytes() {
            const size_t bytes = m_uploadedBytes;
            m_uploadedBytes = 0;
            return bytes;
        }

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
        bool m_perSectionIndexBuffers = false;

        // Accumulated by every slab write; drained once a frame by
        // ConsumeUploadedBytes. Render-thread only, so no atomic needed.
        size_t m_uploadedBytes = 0;

        // Per-section region tracking (which slab + offset)
        struct Region {
            uint32_t slabIndex;
            size_t vertexOffset;
            size_t vertexCount;
            size_t indexOffset;
            size_t indexCount;
            // Per-section index buffer, MC-style (CompiledSectionMesh ->
            // SectionBuffers.getIndexBuffer()). Only used when the pool was
            // initialised with perSectionIndexBuffers; INVALID_BUFFER otherwise
            // and indices live in the slab IBO at indexOffset as before.
            BufferHandle sectionIbo = INVALID_BUFFER;
        };
        std::unordered_map<MegaBufferSectionKey, Region, MegaBufferSectionKeyHash> m_regions;

        // Ranges released by RemoveSection, held back from the free-lists until
        // no in-flight frame can still be drawing them. See RetireFreedRegions.
        struct PendingFree {
            uint32_t slabIndex;
            size_t   vertexOffset;
            size_t   vertexCount;
            size_t   indexOffset;
            size_t   indexCount;
            bool     freeIndices;   // false in per-section-IBO mode
            uint64_t frameFreed;
        };
        std::vector<PendingFree> m_pendingFrees;
        uint64_t m_frameCounter = 0;
        // MAX_FRAMES_IN_FLIGHT is 2, so frame N can still be reading what frame
        // N-1 drew. 3 covers that with a frame to spare, and the cost of being
        // generous is a few hundred KB of briefly-unreusable slab space.
        static constexpr uint64_t kFreeDelayFrames = 3;

        // Slab management
        uint32_t AllocateSlab();
        bool TryUploadToSlab(uint32_t slabIndex, const MegaBufferSectionKey& key,
                             const float* vertexData, size_t vertexCount,
                             const uint16_t* indexData, size_t indexCount);

        // Internal allocation (first-fit with bump fallback, per-slab)
        static bool AllocRegion(std::vector<Slab::FreeBlock>& freeList, size_t& highWater,
                                size_t capacity, size_t count, size_t& outOffset);
        static void FreeRegion(std::vector<Slab::FreeBlock>& freeList,
                               size_t offset, size_t count);

        static constexpr size_t VERTEX_STRIDE = 24;
        // 16-bit indices: section-relative (drawn with baseVertex), halving
        // index memory and GPU fetch bandwidth vs uint32.
        static constexpr size_t INDEX_SIZE = sizeof(uint16_t);
    };

} // namespace Render
