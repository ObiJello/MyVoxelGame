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

    void ChunkMegaBuffer::Initialize(size_t slabVertexCapacity, size_t slabIndexCapacity,
                                     bool perSectionIndexBuffers) {
        if (!m_slabs.empty()) {
            Log::Warning("ChunkMegaBuffer::Initialize called on already-initialized buffer, shutting down first");
            Shutdown();
        }

        m_slabVertexCapacity = slabVertexCapacity;
        m_slabIndexCapacity = slabIndexCapacity;
        m_perSectionIndexBuffers = perSectionIndexBuffers;

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
            // Per-section IBOs are owned by their region, not by a slab, so they
            // have to be released here too or the world teardown leaks one buffer
            // per translucent section.
            for (auto& [key, region] : m_regions) {
                if (region.sectionIbo != INVALID_BUFFER) {
                    g_renderBackend->DestroyBuffer(region.sectionIbo);
                }
            }
            for (auto& slab : m_slabs) {
                if (slab.vbo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.vbo);
                if (slab.ibo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.ibo);
            }
        }
        m_slabs.clear();
        m_regions.clear();
        m_pendingFrees.clear();
        m_frameCounter = 0;
        m_slabVertexCapacity = 0;
        m_slabIndexCapacity = 0;
        m_perSectionIndexBuffers = false;
    }

    BufferHandle ChunkMegaBuffer::GetSectionIndexBuffer(const MegaBufferSectionKey& key) const {
        auto it = m_regions.find(key);
        return it == m_regions.end() ? INVALID_BUFFER : it->second.sectionIbo;
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
        // Per-section mode binds the section's own IBO at draw time instead.
        if (!m_perSectionIndexBuffers) {
            g_renderBackend->BindIndexBuffer(slab.ibo);
        }
    }

    // ========================================================================
    // SECTION MANAGEMENT
    // ========================================================================

    bool ChunkMegaBuffer::UploadSection(const MegaBufferSectionKey& key,
                                         const float* vertexData, size_t vertexCount,
                                         const uint16_t* indexData, size_t indexCount) {
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
                                           const uint16_t* indexData, size_t indexCount) {
        if (!g_renderBackend) return false;
        Slab& slab = m_slabs[slabIndex];

        // Try vertex allocation
        size_t vertexOffset = 0;
        if (!AllocRegion(slab.freeVertexBlocks, slab.vertexHighWater, slab.vboCapacity, vertexCount, vertexOffset))
            return false;

        // Index allocation. In per-section mode the slab IBO is untouched — the
        // section gets its own buffer below — so there is nothing to reserve and
        // nothing to fail on.
        size_t indexOffset = 0;
        if (!m_perSectionIndexBuffers) {
            if (!AllocRegion(slab.freeIndexBlocks, slab.indexHighWater, slab.iboCapacity, indexCount, indexOffset)) {
                // Undo vertex allocation
                FreeRegion(slab.freeVertexBlocks, vertexOffset, vertexCount);
                return false;
            }
        }

        // Upload vertex data
        g_renderBackend->UpdateBuffer(slab.vbo,
                                       vertexOffset * VERTEX_STRIDE,
                                       vertexCount * VERTEX_STRIDE,
                                       vertexData);

        // Upload index data
        BufferHandle sectionIbo = INVALID_BUFFER;
        if (m_perSectionIndexBuffers) {
            sectionIbo = g_renderBackend->CreateBuffer(
                BufferUsage::Index,
                indexCount * INDEX_SIZE,
                indexData,
                BufferAccess::Dynamic);
            if (sectionIbo == INVALID_BUFFER) {
                FreeRegion(slab.freeVertexBlocks, vertexOffset, vertexCount);
                return false;
            }
        } else {
            g_renderBackend->UpdateBuffer(slab.ibo,
                                           indexOffset * INDEX_SIZE,
                                           indexCount * INDEX_SIZE,
                                           indexData);
        }

        m_uploadedBytes += vertexCount * VERTEX_STRIDE + indexCount * INDEX_SIZE;

        // Store region
        m_regions[key] = {slabIndex, vertexOffset, vertexCount, indexOffset, indexCount, sectionIbo};
        slab.sectionCount++;
        return true;
    }

    bool ChunkMegaBuffer::UpdateSectionIndices(const MegaBufferSectionKey& key,
                                               const uint16_t* indexData, size_t indexCount) {
        if (!g_renderBackend || !indexData) return false;

        auto it = m_regions.find(key);
        if (it == m_regions.end()) return false;

        const Region& region = it->second;
        // Same-size overwrite only. A re-sort permutes quads, so the count is
        // invariant; anything else would need a fresh allocation and is a bug
        // in the caller rather than something to silently accommodate.
        if (indexCount != region.indexCount) return false;
        if (region.slabIndex >= m_slabs.size()) return false;

        // Per-section mode writes a buffer only this section draws from, so the
        // driver has no in-flight draws to serialise against — that stall is the
        // entire reason this mode exists.
        if (m_perSectionIndexBuffers) {
            if (region.sectionIbo == INVALID_BUFFER) return false;
            g_renderBackend->UpdateBuffer(region.sectionIbo, 0,
                                          indexCount * INDEX_SIZE, indexData);
        } else {
            // Unsynchronised: this is a same-length permutation of the section's
            // own quad range (enforced above), so a torn read is a mix of two
            // valid orderings, never an invalid index. See
            // RenderBackend::UpdateBufferUnsynchronized.
            g_renderBackend->UpdateBufferUnsynchronized(
                m_slabs[region.slabIndex].ibo,
                region.indexOffset * INDEX_SIZE,
                indexCount * INDEX_SIZE,
                indexData);
        }
        m_uploadedBytes += indexCount * INDEX_SIZE;
        return true;
    }

    void ChunkMegaBuffer::RemoveSection(const MegaBufferSectionKey& key) {
        auto it = m_regions.find(key);
        if (it == m_regions.end()) return;

        const Region& region = it->second;
        // Per-section IBO is owned by the region, so it dies with it. Nothing was
        // reserved in the slab index free-list in that mode, so do not hand a
        // never-allocated range back to it.
        if (region.sectionIbo != INVALID_BUFFER && g_renderBackend) {
            g_renderBackend->DestroyBuffer(region.sectionIbo);
        }
        if (region.slabIndex < m_slabs.size()) {
            Slab& slab = m_slabs[region.slabIndex];
            // NOT returned to the free-list yet — an in-flight frame may still
            // be drawing this range, and handing it straight back would let the
            // very next UploadSection overwrite it. See RetireFreedRegions.
            m_pendingFrees.push_back({region.slabIndex,
                                      region.vertexOffset, region.vertexCount,
                                      region.indexOffset, region.indexCount,
                                      !m_perSectionIndexBuffers,
                                      m_frameCounter});
            if (slab.sectionCount > 0) slab.sectionCount--;
        }
        m_regions.erase(it);
    }

    void ChunkMegaBuffer::RetireFreedRegions() {
        m_frameCounter++;
        if (m_pendingFrees.empty()) return;

        size_t keep = 0;
        for (const PendingFree& p : m_pendingFrees) {
            if (m_frameCounter - p.frameFreed < kFreeDelayFrames) {
                m_pendingFrees[keep++] = p;   // still too young to reuse
                continue;
            }
            if (p.slabIndex < m_slabs.size()) {
                Slab& slab = m_slabs[p.slabIndex];
                FreeRegion(slab.freeVertexBlocks, p.vertexOffset, p.vertexCount);
                if (p.freeIndices) {
                    FreeRegion(slab.freeIndexBlocks, p.indexOffset, p.indexCount);
                }
            }
        }
        m_pendingFrees.resize(keep);
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
                // Deferred: the slab emptied this frame, but the PREVIOUS
                // frame's command stream may still reference these buffers.
                // GL's default is an immediate delete (driver refcounts
                // pending commands); Vulkan queues the handles on the
                // current frame's deletion queue and frees them after its
                // fence — destroying immediately there is use-after-free.
                if (slab.vbo != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slab.vbo);
                if (slab.ibo != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slab.ibo);
            }
            Log::Debug("ChunkMegaBuffer: freed empty slab %zu", m_slabs.size() - 1);
            m_slabs.pop_back();
            removed = true;
        }

        // Drop parked ranges belonging to slabs that just went away. The bounds
        // check in RetireFreedRegions would skip them today, but only until a
        // new slab is allocated into the same index — then a stale range would
        // be returned to a DIFFERENT slab's free-list and hand out memory that
        // is already in use. Purge them here so that cannot happen.
        if (removed) {
            const uint32_t slabCount = static_cast<uint32_t>(m_slabs.size());
            m_pendingFrees.erase(
                std::remove_if(m_pendingFrees.begin(), m_pendingFrees.end(),
                               [slabCount](const PendingFree& p) { return p.slabIndex >= slabCount; }),
                m_pendingFrees.end());
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
