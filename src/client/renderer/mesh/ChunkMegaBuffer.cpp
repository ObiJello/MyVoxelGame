// File: src/client/renderer/mesh/ChunkMegaBuffer.cpp
#include "ChunkMegaBuffer.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <algorithm>
#include <cstring>

namespace Render {

    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    ChunkMegaBuffer::~ChunkMegaBuffer() {
        Shutdown();
    }

    void ChunkMegaBuffer::Initialize(size_t initialVertexCapacity, size_t initialIndexCapacity) {
        if (m_vbo != 0) {
            Log::Warning("ChunkMegaBuffer::Initialize called on already-initialized buffer, shutting down first");
            Shutdown();
        }

        m_vboCapacity = initialVertexCapacity;
        m_iboCapacity = initialIndexCapacity;

        // Create VBO with initial capacity
        glGenBuffers(1, &m_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(m_vboCapacity * VERTEX_STRIDE),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Create IBO with initial capacity
        glGenBuffers(1, &m_ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(m_iboCapacity * INDEX_SIZE),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        // No per-buffer VAO — the shared block VAO in ClientMeshManager defines
        // the vertex format once.  Mega-buffers are switched at render time with
        // BindBuffers() (glBindVertexBuffer + glBindBuffer), which is a cheap
        // pointer swap that avoids GPU pipeline flushes on macOS.

        Log::Info("ChunkMegaBuffer initialized: VBO capacity=%zu verts (%.1f MB), IBO capacity=%zu indices (%.1f MB)",
                  m_vboCapacity,
                  static_cast<double>(m_vboCapacity * VERTEX_STRIDE) / (1024.0 * 1024.0),
                  m_iboCapacity,
                  static_cast<double>(m_iboCapacity * INDEX_SIZE) / (1024.0 * 1024.0));
    }

    void ChunkMegaBuffer::Shutdown() {
        if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
        if (m_ibo) { glDeleteBuffers(1, &m_ibo); m_ibo = 0; }

        m_regions.clear();
        m_freeVertexBlocks.clear();
        m_freeIndexBlocks.clear();
        m_vboCapacity = 0;
        m_iboCapacity = 0;
        m_vertexHighWater = 0;
        m_indexHighWater = 0;
        m_freedVertices = 0;
        m_freedIndices = 0;
    }

    // ========================================================================
    // BINDING (GL_ARB_vertex_attrib_binding)
    // ========================================================================

    void ChunkMegaBuffer::BindBuffers(bool useVertexAttribBinding) const {
        if (useVertexAttribBinding) {
            // Separate path (Windows/Linux): just switch VBO on binding point 0.
            // The vertex format is baked into the shared VAO — no re-setup needed.
            glBindVertexBuffer(0, m_vbo, 0, static_cast<GLsizei>(VERTEX_STRIDE));
        } else {
            // Emulated path (macOS): rebind VBO and re-set attribute pointers.
            // Same VAO stays bound — no glBindVertexArray = no GPU pipeline flush.
            // Matches Minecraft's VertexArrayCache.Emulated.setupCombinedAttributes.
            glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(VERTEX_STRIDE),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(0)));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(VERTEX_STRIDE),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(3 * sizeof(float))));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  static_cast<GLsizei>(VERTEX_STRIDE),
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(5 * sizeof(float))));
        }

        // IBO is per-VAO state — always rebind
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    }

    // ========================================================================
    // SECTION MANAGEMENT
    // ========================================================================

    bool ChunkMegaBuffer::UploadSection(const MegaBufferSectionKey& key,
                                         const float* vertexData, size_t vertexCount,
                                         const uint32_t* indexData, size_t indexCount) {
        PROFILE_ZONE;
        if (vertexCount == 0 || indexCount == 0) return false;
        if (!vertexData || !indexData) return false;

        // If section already exists, remove it first (re-upload)
        if (m_regions.count(key)) {
            RemoveSection(key);
        }

        // Allocate vertex region
        size_t vertexOffset = 0;
        if (!AllocRegion(m_freeVertexBlocks, m_vertexHighWater, m_vboCapacity, vertexCount, vertexOffset)) {
            // Need to grow VBO
            size_t needed = m_vertexHighWater + vertexCount;
            // Grow by 1.5× (not 2×) to reduce memory waste; minimum growth of 256K vertices
            size_t newCap = std::max(m_vboCapacity + std::max(m_vboCapacity / 2, size_t(256000)), needed);
            GrowVBO(newCap);
            if (!AllocRegion(m_freeVertexBlocks, m_vertexHighWater, m_vboCapacity, vertexCount, vertexOffset)) {
                Log::Warning("ChunkMegaBuffer: vertex allocation failed after grow (requested %zu)", vertexCount);
                return false;
            }
        }

        // Allocate index region
        size_t indexOffset = 0;
        if (!AllocRegion(m_freeIndexBlocks, m_indexHighWater, m_iboCapacity, indexCount, indexOffset)) {
            // Need to grow IBO
            size_t needed = m_indexHighWater + indexCount;
            // Grow by 1.5× (not 2×) to reduce memory waste; minimum growth of 512K indices
            size_t newCap = std::max(m_iboCapacity + std::max(m_iboCapacity / 2, size_t(512000)), needed);
            GrowIBO(newCap);
            if (!AllocRegion(m_freeIndexBlocks, m_indexHighWater, m_iboCapacity, indexCount, indexOffset)) {
                // Undo vertex allocation
                FreeRegion(m_freeVertexBlocks, m_freedVertices, vertexOffset, vertexCount);
                Log::Warning("ChunkMegaBuffer: index allocation failed after grow (requested %zu)", indexCount);
                return false;
            }
        }

        // Upload vertex data to VBO
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER,
                        static_cast<GLintptr>(vertexOffset * VERTEX_STRIDE),
                        static_cast<GLsizeiptr>(vertexCount * VERTEX_STRIDE),
                        vertexData);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Upload index data to IBO (indices are 0-based; baseVertex handles offset at draw time)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
                        static_cast<GLintptr>(indexOffset * INDEX_SIZE),
                        static_cast<GLsizeiptr>(indexCount * INDEX_SIZE),
                        indexData);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        // Store region
        m_regions[key] = {vertexOffset, vertexCount, indexOffset, indexCount};
        return true;
    }

    void ChunkMegaBuffer::RemoveSection(const MegaBufferSectionKey& key) {
        auto it = m_regions.find(key);
        if (it == m_regions.end()) return;

        const Region& region = it->second;
        FreeRegion(m_freeVertexBlocks, m_freedVertices, region.vertexOffset, region.vertexCount);
        FreeRegion(m_freeIndexBlocks, m_freedIndices, region.indexOffset, region.indexCount);
        m_regions.erase(it);
    }

    bool ChunkMegaBuffer::HasSection(const MegaBufferSectionKey& key) const {
        return m_regions.count(key) > 0;
    }

    // ========================================================================
    // DRAW COMMANDS
    // ========================================================================

    bool ChunkMegaBuffer::GetDrawCommand(const MegaBufferSectionKey& key, DrawCommand& outCmd) const {
        auto it = m_regions.find(key);
        if (it == m_regions.end()) return false;

        const Region& r = it->second;
        outCmd.indexCount = static_cast<GLsizei>(r.indexCount);
        outCmd.indexByteOffset = r.indexOffset * INDEX_SIZE;
        outCmd.baseVertex = static_cast<GLint>(r.vertexOffset);
        return true;
    }

    // ========================================================================
    // STATISTICS
    // ========================================================================

    size_t ChunkMegaBuffer::GetMemoryUsageBytes() const {
        return m_vboCapacity * VERTEX_STRIDE + m_iboCapacity * INDEX_SIZE;
    }

    float ChunkMegaBuffer::GetFragmentation() const {
        if (m_vertexHighWater == 0) return 0.0f;

        // Count actual vertices in use (sum of all live region vertex counts)
        size_t liveVertices = 0;
        for (const auto& [key, region] : m_regions) {
            liveVertices += region.vertexCount;
        }

        // Fragmentation = fraction of high-water space that's wasted (freed but not reclaimed)
        if (liveVertices >= m_vertexHighWater) return 0.0f;
        return 1.0f - static_cast<float>(liveVertices) / static_cast<float>(m_vertexHighWater);
    }

    // ========================================================================
    // DEFRAGMENTATION
    // ========================================================================

    void ChunkMegaBuffer::CompactIfNeeded(float threshold) {
        PROFILE_ZONE;
        if (GetFragmentation() < threshold) return;
        if (m_regions.empty()) return;

        Log::Debug("ChunkMegaBuffer: compacting (fragmentation=%.1f%%, %zu sections)",
                   GetFragmentation() * 100.0f, m_regions.size());

        // Calculate total live data
        size_t totalVertices = 0;
        size_t totalIndices = 0;
        for (const auto& [key, region] : m_regions) {
            totalVertices += region.vertexCount;
            totalIndices += region.indexCount;
        }

        // Create new buffers
        // Allocate 1.25× the live data size — enough headroom for new uploads
        // before the next compaction, without wasting 2× memory
        size_t newVboCapacity = totalVertices + totalVertices / 4;
        size_t newIboCapacity = totalIndices + totalIndices / 4;

        GLuint newVbo = 0;
        glGenBuffers(1, &newVbo);
        glBindBuffer(GL_ARRAY_BUFFER, newVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(newVboCapacity * VERTEX_STRIDE),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        GLuint newIbo = 0;
        glGenBuffers(1, &newIbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(newIboCapacity * INDEX_SIZE),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        // Copy each region contiguously into the new buffers
        size_t newVertexOffset = 0;
        size_t newIndexOffset = 0;

        for (auto& [key, region] : m_regions) {
            // Copy vertices: old VBO -> new VBO
            glBindBuffer(GL_COPY_READ_BUFFER, m_vbo);
            glBindBuffer(GL_COPY_WRITE_BUFFER, newVbo);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                static_cast<GLintptr>(region.vertexOffset * VERTEX_STRIDE),
                                static_cast<GLintptr>(newVertexOffset * VERTEX_STRIDE),
                                static_cast<GLsizeiptr>(region.vertexCount * VERTEX_STRIDE));

            // Copy indices: old IBO -> new IBO
            glBindBuffer(GL_COPY_READ_BUFFER, m_ibo);
            glBindBuffer(GL_COPY_WRITE_BUFFER, newIbo);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                static_cast<GLintptr>(region.indexOffset * INDEX_SIZE),
                                static_cast<GLintptr>(newIndexOffset * INDEX_SIZE),
                                static_cast<GLsizeiptr>(region.indexCount * INDEX_SIZE));

            // Update region to new offsets
            region.vertexOffset = newVertexOffset;
            region.indexOffset = newIndexOffset;
            newVertexOffset += region.vertexCount;
            newIndexOffset += region.indexCount;
        }

        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

        // Swap buffers
        glDeleteBuffers(1, &m_vbo);
        glDeleteBuffers(1, &m_ibo);
        m_vbo = newVbo;
        m_ibo = newIbo;
        m_vboCapacity = newVboCapacity;
        m_iboCapacity = newIboCapacity;
        m_vertexHighWater = newVertexOffset;
        m_indexHighWater = newIndexOffset;

        // Clear free lists (everything is packed tightly now)
        m_freeVertexBlocks.clear();
        m_freeIndexBlocks.clear();
        m_freedVertices = 0;
        m_freedIndices = 0;

        // No VAO recreation needed — the shared block VAO uses
        // GL_ARB_vertex_attrib_binding; new buffers are picked up at render
        // time via BindBuffers().

        Log::Debug("ChunkMegaBuffer: compaction complete (%zu verts, %zu indices packed)",
                   newVertexOffset, newIndexOffset);
    }

    // ========================================================================
    // INTERNAL ALLOCATION
    // ========================================================================

    bool ChunkMegaBuffer::AllocRegion(std::vector<FreeBlock>& freeList, size_t& highWater,
                                       size_t capacity, size_t count, size_t& outOffset) {
        // Try free-list first (first-fit)
        for (auto it = freeList.begin(); it != freeList.end(); ++it) {
            if (it->size >= count) {
                outOffset = it->offset;
                if (it->size == count) {
                    freeList.erase(it);
                } else {
                    it->offset += count;
                    it->size -= count;
                }
                return true;
            }
        }

        // Fall back to high-water mark (bump allocation)
        if (highWater + count <= capacity) {
            outOffset = highWater;
            highWater += count;
            return true;
        }

        return false;  // Out of space
    }

    void ChunkMegaBuffer::FreeRegion(std::vector<FreeBlock>& freeList, size_t& freedTotal,
                                      size_t offset, size_t count) {
        freedTotal += count;

        // Insert in sorted order (by offset) for coalescing
        auto insertPos = std::lower_bound(freeList.begin(), freeList.end(), offset,
            [](const FreeBlock& block, size_t off) { return block.offset < off; });
        insertPos = freeList.insert(insertPos, {offset, count});

        // Coalesce with next block
        auto next = std::next(insertPos);
        if (next != freeList.end() && insertPos->offset + insertPos->size == next->offset) {
            insertPos->size += next->size;
            freeList.erase(next);
        }

        // Coalesce with previous block
        if (insertPos != freeList.begin()) {
            auto prev = std::prev(insertPos);
            if (prev->offset + prev->size == insertPos->offset) {
                prev->size += insertPos->size;
                freeList.erase(insertPos);
            }
        }
    }

    // ========================================================================
    // BUFFER GROWTH
    // ========================================================================

    void ChunkMegaBuffer::GrowVBO(size_t newCapacity) {
        Log::Debug("ChunkMegaBuffer: growing VBO %zu -> %zu vertices (%.1f MB -> %.1f MB)",
                   m_vboCapacity, newCapacity,
                   static_cast<double>(m_vboCapacity * VERTEX_STRIDE) / (1024.0 * 1024.0),
                   static_cast<double>(newCapacity * VERTEX_STRIDE) / (1024.0 * 1024.0));

        GLuint newVbo = 0;
        glGenBuffers(1, &newVbo);
        glBindBuffer(GL_ARRAY_BUFFER, newVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(newCapacity * VERTEX_STRIDE),
                     nullptr, GL_DYNAMIC_DRAW);

        // Copy existing data
        if (m_vbo && m_vertexHighWater > 0) {
            glBindBuffer(GL_COPY_READ_BUFFER, m_vbo);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ARRAY_BUFFER, 0, 0,
                                static_cast<GLsizeiptr>(m_vertexHighWater * VERTEX_STRIDE));
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Replace old VBO
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        m_vbo = newVbo;
        m_vboCapacity = newCapacity;

        // No VAO recreation needed — the shared block VAO uses
        // GL_ARB_vertex_attrib_binding; the VBO is switched at render time
        // via BindBuffers() (glBindVertexBuffer).
    }

    void ChunkMegaBuffer::GrowIBO(size_t newCapacity) {
        Log::Debug("ChunkMegaBuffer: growing IBO %zu -> %zu indices (%.1f MB -> %.1f MB)",
                   m_iboCapacity, newCapacity,
                   static_cast<double>(m_iboCapacity * INDEX_SIZE) / (1024.0 * 1024.0),
                   static_cast<double>(newCapacity * INDEX_SIZE) / (1024.0 * 1024.0));

        GLuint newIbo = 0;
        glGenBuffers(1, &newIbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(newCapacity * INDEX_SIZE),
                     nullptr, GL_DYNAMIC_DRAW);

        // Copy existing data
        if (m_ibo && m_indexHighWater > 0) {
            glBindBuffer(GL_COPY_READ_BUFFER, m_ibo);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ELEMENT_ARRAY_BUFFER, 0, 0,
                                static_cast<GLsizeiptr>(m_indexHighWater * INDEX_SIZE));
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        // Replace old IBO
        if (m_ibo) glDeleteBuffers(1, &m_ibo);
        m_ibo = newIbo;
        m_iboCapacity = newCapacity;

        // No VAO recreation needed — IBO is re-bound at render time via
        // BindBuffers() (glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...)).
    }

} // namespace Render
