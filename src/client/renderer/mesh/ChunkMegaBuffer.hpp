// File: src/client/renderer/mesh/ChunkMegaBuffer.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include <glad/glad.h>
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
    // GPU MEGA-BUFFER
    // ========================================================================
    //
    // Packs all chunk section vertices/indices for one render layer into a
    // single large VBO + IBO, enabling multi-draw rendering via
    // glMultiDrawElementsBaseVertex (3 draw calls instead of 3000+).
    //
    // One ChunkMegaBuffer is used per render layer (opaque, cutout, translucent).
    // Sections upload their vertex/index data into the mega-buffer; indices are
    // stored 0-based per section and baseVertex offset is used at draw time.
    //
    // Free-list allocator manages regions with first-fit and coalescing.
    // Buffers grow automatically when capacity is exceeded.
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

        void Initialize(size_t initialVertexCapacity = 500000, size_t initialIndexCapacity = 1000000);
        void Shutdown();
        bool IsInitialized() const { return m_vbo != 0; }

        // ========================================================================
        // SECTION MANAGEMENT
        // ========================================================================

        // Upload a section's mesh data into the mega-buffer.
        // vertexData: raw float array (24 bytes per vertex)
        // indexData: uint32_t indices (0-based for this section)
        // Returns false if allocation fails after grow attempt.
        bool UploadSection(const MegaBufferSectionKey& key,
                           const float* vertexData, size_t vertexCount,
                           const uint32_t* indexData, size_t indexCount);

        // Remove a section and free its region
        void RemoveSection(const MegaBufferSectionKey& key);

        // Check if a section exists
        bool HasSection(const MegaBufferSectionKey& key) const;

        // ========================================================================
        // DRAW COMMANDS
        // ========================================================================

        struct DrawCommand {
            GLsizei indexCount;
            size_t indexByteOffset;   // Byte offset into IBO
            GLint baseVertex;         // Added to each index by GL
        };

        // Get draw command for a specific section (returns false if not found)
        bool GetDrawCommand(const MegaBufferSectionKey& key, DrawCommand& outCmd) const;

        // ========================================================================
        // BINDING
        // ========================================================================

        // Bind this mega-buffer's VBO and IBO into the currently-bound shared VAO.
        // Two paths (matching Minecraft's VertexArrayCache):
        //   useVertexAttribBinding=true:  glBindVertexBuffer (Separate path, Win/Linux)
        //   useVertexAttribBinding=false: glBindBuffer + glVertexAttribPointer (Emulated path, macOS)
        // Both avoid glBindVertexArray switches = no GPU pipeline flush.
        void BindBuffers(bool useVertexAttribBinding) const;

        // Raw buffer accessors (for external systems that need the GL names)
        GLuint GetVBO() const { return m_vbo; }
        GLuint GetIBO() const { return m_ibo; }

        // ========================================================================
        // STATISTICS
        // ========================================================================

        size_t GetSectionCount() const { return m_regions.size(); }
        size_t GetUsedVertices() const { return m_vertexHighWater; }
        size_t GetUsedIndices() const { return m_indexHighWater; }
        size_t GetVertexCapacity() const { return m_vboCapacity; }
        size_t GetIndexCapacity() const { return m_iboCapacity; }
        size_t GetMemoryUsageBytes() const;
        float GetFragmentation() const;

        // ========================================================================
        // DEFRAGMENTATION
        // ========================================================================

        // Compact the buffer if fragmentation exceeds the threshold (0.0 = none, 1.0 = all freed).
        // Performs a full rebuild: copies all live regions contiguously into new buffers.
        void CompactIfNeeded(float threshold = 0.5f);

    private:
        // OpenGL resources (no per-buffer VAO — the shared block VAO in
        // ClientMeshManager defines the vertex format once; mega-buffers are
        // switched with glBindVertexBuffer, avoiding GPU pipeline flushes).
        GLuint m_vbo = 0;
        GLuint m_ibo = 0;

        // Capacities (in elements, not bytes)
        size_t m_vboCapacity = 0;  // Max vertices
        size_t m_iboCapacity = 0;  // Max indices

        // High-water marks (next free position at the end)
        size_t m_vertexHighWater = 0;
        size_t m_indexHighWater = 0;

        // Per-section region tracking
        struct Region {
            size_t vertexOffset;
            size_t vertexCount;
            size_t indexOffset;
            size_t indexCount;
        };
        std::unordered_map<MegaBufferSectionKey, Region, MegaBufferSectionKeyHash> m_regions;

        // Free-list (sorted by offset for coalescing)
        struct FreeBlock {
            size_t offset;
            size_t size;
        };
        std::vector<FreeBlock> m_freeVertexBlocks;
        std::vector<FreeBlock> m_freeIndexBlocks;

        // Total freed (for fragmentation calculation)
        size_t m_freedVertices = 0;
        size_t m_freedIndices = 0;

        // Internal allocation (first-fit with bump fallback)
        bool AllocRegion(std::vector<FreeBlock>& freeList, size_t& highWater, size_t capacity,
                         size_t count, size_t& outOffset);
        void FreeRegion(std::vector<FreeBlock>& freeList, size_t& freedTotal, size_t offset, size_t count);

        // Buffer management
        void GrowVBO(size_t newCapacity);
        void GrowIBO(size_t newCapacity);

        static constexpr size_t VERTEX_STRIDE = 24;  // 24 bytes per vertex
        static constexpr size_t INDEX_SIZE = sizeof(uint32_t);
    };

} // namespace Render
