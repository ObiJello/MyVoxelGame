// File: src/client/renderer/mesh/ChunkMegaBuffer.cpp
#include "ChunkMegaBuffer.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <algorithm>

namespace Render {

    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    ChunkMegaBuffer::~ChunkMegaBuffer() {
        Shutdown();
    }

    void ChunkMegaBuffer::Initialize(size_t slabVertexCapacity, size_t slabIndexCapacity) {
        if (!m_slabs.empty()) {
            Log::Warning("ChunkMegaBuffer::Initialize called on already-initialized buffer, shutting down first");
            Shutdown();
        }

        m_slabVertexCapacity = slabVertexCapacity;
        m_slabIndexCapacity = slabIndexCapacity;

        // Allocate the first slab
        AllocateSlab();

        Log::Info("ChunkMegaBuffer initialized: slab size=%zu verts (%.1f MB) / %zu indices (%.1f MB)",
                  m_slabVertexCapacity,
                  static_cast<double>(m_slabVertexCapacity * VERTEX_STRIDE) / (1024.0 * 1024.0),
                  m_slabIndexCapacity,
                  static_cast<double>(m_slabIndexCapacity * INDEX_SIZE) / (1024.0 * 1024.0));
    }

    void ChunkMegaBuffer::Shutdown() {
        if (g_renderBackend) {
            for (auto& slab : m_slabs) {
                if (slab.vbo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.vbo);
                if (slab.ibo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.ibo);
            }
        }
        m_slabs.clear();
        m_regions.clear();
        m_slabVertexCapacity = 0;
        m_slabIndexCapacity = 0;
    }

    uint32_t ChunkMegaBuffer::AllocateSlab() {
        PROFILE_ZONE;
        if (!g_renderBackend) {
            Log::Error("ChunkMegaBuffer::AllocateSlab: no render backend");
            return 0;
        }

        Slab slab;
        slab.vboCapacity = m_slabVertexCapacity;
        slab.iboCapacity = m_slabIndexCapacity;

        slab.vbo = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex,
            slab.vboCapacity * VERTEX_STRIDE,
            nullptr,
            BufferAccess::Dynamic);

        slab.ibo = g_renderBackend->CreateBuffer(
            BufferUsage::Index,
            slab.iboCapacity * INDEX_SIZE,
            nullptr,
            BufferAccess::Dynamic);

        uint32_t index = static_cast<uint32_t>(m_slabs.size());
        m_slabs.push_back(std::move(slab));

        Log::Debug("ChunkMegaBuffer: allocated slab %u (%.1f MB VBO + %.1f MB IBO)",
                   index,
                   static_cast<double>(m_slabVertexCapacity * VERTEX_STRIDE) / (1024.0 * 1024.0),
                   static_cast<double>(m_slabIndexCapacity * INDEX_SIZE) / (1024.0 * 1024.0));
        return index;
    }

    // ========================================================================
    // SLAB BINDING
    // ========================================================================

    void ChunkMegaBuffer::BindSlab(uint32_t slabIndex) const {
        if (slabIndex >= m_slabs.size() || !g_renderBackend) return;
        const Slab& slab = m_slabs[slabIndex];

        g_renderBackend->BindVertexBuffer(slab.vbo, static_cast<uint32_t>(VERTEX_STRIDE));
        g_renderBackend->BindIndexBuffer(slab.ibo);
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

        // Try to fit in an existing slab (last first — most likely to have space)
        for (int i = static_cast<int>(m_slabs.size()) - 1; i >= 0; i--) {
            if (TryUploadToSlab(static_cast<uint32_t>(i), key, vertexData, vertexCount, indexData, indexCount))
                return true;
        }

        // No slab has space — allocate a new one (<1ms, zero copy)
        uint32_t newSlab = AllocateSlab();
        return TryUploadToSlab(newSlab, key, vertexData, vertexCount, indexData, indexCount);
    }

    bool ChunkMegaBuffer::TryUploadToSlab(uint32_t slabIndex, const MegaBufferSectionKey& key,
                                           const float* vertexData, size_t vertexCount,
                                           const uint32_t* indexData, size_t indexCount) {
        if (!g_renderBackend) return false;
        Slab& slab = m_slabs[slabIndex];

        // Try vertex allocation
        size_t vertexOffset = 0;
        if (!AllocRegion(slab.freeVertexBlocks, slab.vertexHighWater, slab.vboCapacity, vertexCount, vertexOffset))
            return false;

        // Try index allocation
        size_t indexOffset = 0;
        if (!AllocRegion(slab.freeIndexBlocks, slab.indexHighWater, slab.iboCapacity, indexCount, indexOffset)) {
            // Undo vertex allocation
            FreeRegion(slab.freeVertexBlocks, vertexOffset, vertexCount);
            return false;
        }

        // Upload vertex data
        g_renderBackend->UpdateBuffer(slab.vbo,
                                       vertexOffset * VERTEX_STRIDE,
                                       vertexCount * VERTEX_STRIDE,
                                       vertexData);

        // Upload index data
        g_renderBackend->UpdateBuffer(slab.ibo,
                                       indexOffset * INDEX_SIZE,
                                       indexCount * INDEX_SIZE,
                                       indexData);

        // Store region
        m_regions[key] = {slabIndex, vertexOffset, vertexCount, indexOffset, indexCount};
        slab.sectionCount++;
        return true;
    }

    void ChunkMegaBuffer::RemoveSection(const MegaBufferSectionKey& key) {
        auto it = m_regions.find(key);
        if (it == m_regions.end()) return;

        const Region& region = it->second;
        if (region.slabIndex < m_slabs.size()) {
            Slab& slab = m_slabs[region.slabIndex];
            FreeRegion(slab.freeVertexBlocks, region.vertexOffset, region.vertexCount);
            FreeRegion(slab.freeIndexBlocks, region.indexOffset, region.indexCount);
            if (slab.sectionCount > 0) slab.sectionCount--;
        }
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
        outCmd.indexCount = static_cast<int32_t>(r.indexCount);
        outCmd.indexByteOffset = r.indexOffset * INDEX_SIZE;
        outCmd.baseVertex = static_cast<int32_t>(r.vertexOffset);
        outCmd.slabIndex = r.slabIndex;
        return true;
    }

    // ========================================================================
    // STATISTICS
    // ========================================================================

    size_t ChunkMegaBuffer::GetMemoryUsageBytes() const {
        size_t total = 0;
        for (const auto& slab : m_slabs) {
            total += slab.vboCapacity * VERTEX_STRIDE + slab.iboCapacity * INDEX_SIZE;
        }
        return total;
    }

    size_t ChunkMegaBuffer::GetTotalVertexCapacity() const {
        size_t total = 0;
        for (const auto& slab : m_slabs) total += slab.vboCapacity;
        return total;
    }

    size_t ChunkMegaBuffer::GetTotalIndexCapacity() const {
        size_t total = 0;
        for (const auto& slab : m_slabs) total += slab.iboCapacity;
        return total;
    }

    size_t ChunkMegaBuffer::GetUsedVertices() const {
        size_t total = 0;
        for (const auto& [key, region] : m_regions) total += region.vertexCount;
        return total;
    }

    size_t ChunkMegaBuffer::GetUsedIndices() const {
        size_t total = 0;
        for (const auto& [key, region] : m_regions) total += region.indexCount;
        return total;
    }

    // ========================================================================
    // MAINTENANCE
    // ========================================================================

    bool ChunkMegaBuffer::CompactIfNeeded(float) {
        // With slab pool, "compaction" is just deleting empty slabs.
        // We never copy data between slabs — free-list reuse handles fragmentation.
        // Empty slabs at the end of the vector can be safely removed.
        // Interior slabs can't be removed without invalidating indices in cached draw commands.
        bool removed = false;
        while (!m_slabs.empty() && m_slabs.back().sectionCount == 0 && m_slabs.size() > 1) {
            Slab& slab = m_slabs.back();
            if (g_renderBackend) {
                if (slab.vbo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.vbo);
                if (slab.ibo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.ibo);
            }
            Log::Debug("ChunkMegaBuffer: freed empty slab %zu", m_slabs.size() - 1);
            m_slabs.pop_back();
            removed = true;
        }
        return removed;
    }

    // ========================================================================
    // INTERNAL ALLOCATION
    // ========================================================================

    bool ChunkMegaBuffer::AllocRegion(std::vector<Slab::FreeBlock>& freeList, size_t& highWater,
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

        return false;  // Slab is full
    }

    void ChunkMegaBuffer::FreeRegion(std::vector<Slab::FreeBlock>& freeList,
                                      size_t offset, size_t count) {
        // Insert in sorted order (by offset) for coalescing
        auto insertPos = std::lower_bound(freeList.begin(), freeList.end(), offset,
            [](const Slab::FreeBlock& block, size_t off) { return block.offset < off; });
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

} // namespace Render
