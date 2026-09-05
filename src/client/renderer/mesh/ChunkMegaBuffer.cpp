// File: src/client/renderer/mesh/ChunkMegaBuffer.cpp
#include "ChunkMegaBuffer.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/Vertex.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>

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

        static_assert(VERTEX_STRIDE == sizeof(TerrainVertex), "VERTEX_STRIDE must match TerrainVertex");
        static_assert(kSlotsPerSlab - 1 <= TerrainVertex::kSlotMask, "slot rows must fit TerrainVertex::slot");

        // Section-origin tables for every slab this pool can ever own, in one
        // host-visible buffer (see the header). Created before the first slab
        // so BindSlab always has something to bind.
        if (g_renderBackend) {
            m_originsUbo = g_renderBackend->CreateBuffer(
                BufferUsage::Uniform, kMaxSlabs * kSlotBytes, nullptr, BufferAccess::Dynamic);
            if (m_originsUbo == INVALID_BUFFER) {
                Log::Error("ChunkMegaBuffer: failed to create the section-origin table");
            }
        }

        // Allocate the first slab
        AllocateSlab();

        Log::Info("ChunkMegaBuffer initialized: slab size=%zu verts (%.1f MB) / %zu indices (%.1f MB, uint32 absolute)",
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
                // The buffer texture views the VBO: it goes first.
                if (slab.faceMapTex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(slab.faceMapTex);
                if (slab.vbo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.vbo);
                if (slab.ibo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.ibo);
            }
            if (m_originsUbo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(m_originsUbo);
        }
        m_originsUbo = INVALID_BUFFER;
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

        if (m_slabs.size() >= kMaxSlabs) {
            // The origin-table buffer is sized for kMaxSlabs (2 GB of vertex
            // data at the opaque slab size — never reached in practice).
            Log::Error("ChunkMegaBuffer: slab limit (%u) reached; section not uploaded", kMaxSlabs);
            return UINT32_MAX;
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

        // The face map: the same bytes as the VBO, seen by the fragment
        // shader as RGBA16 texels (one record each). One view per slab,
        // bound with the slab.
        slab.faceMapTex = g_renderBackend->CreateBufferTexture(slab.vbo, TextureFormat::RGBA16);
        if (slab.faceMapTex == INVALID_TEXTURE) {
            Log::Error("ChunkMegaBuffer: no buffer texture for the face map — "
                       "greedy-merged rectangles will read garbage records");
        }

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
        // This slab's section-origin table, read by the terrain vertex shader.
        if (m_originsUbo != INVALID_BUFFER) {
            g_renderBackend->BindUniformBuffer(m_originsUbo, slabIndex * kSlotBytes, kSlotBytes);
        }
        // ... and its face map, read by the terrain fragment shaders
        // (uFaceMap, texture unit 2 — ChunkRenderer::BindSpriteTable sets
        // the sampler uniform).
        if (slab.faceMapTex != INVALID_TEXTURE) {
            g_renderBackend->BindTexture(slab.faceMapTex, 2);
        }
    }

    // ========================================================================
    // SECTION MANAGEMENT
    // ========================================================================

    // uint16 section-relative -> uint32 absolute, into m_indexScratch. The
    // add cannot overflow: a slab holds at most m_slabVertexCapacity vertices
    // (hundreds of thousands) and a section layer at most 65,536.
    static void ConvertToAbsolute(std::vector<uint32_t>& scratch,
                                  const uint16_t* indexData, size_t indexCount,
                                  size_t vertexOffset) {
        scratch.resize(indexCount);
        const uint32_t base = static_cast<uint32_t>(vertexOffset);
        for (size_t i = 0; i < indexCount; ++i) {
            scratch[i] = base + static_cast<uint32_t>(indexData[i]);
        }
    }

    bool ChunkMegaBuffer::UploadSection(const MegaBufferSectionKey& key,
                                         const float* vertexData, size_t vertexCount,
                                         const uint16_t* indexData, size_t indexCount,
                                         const uint32_t* faceMap, size_t faceMapTexels) {
        PROFILE_ZONE;
        if (vertexCount == 0 || indexCount == 0) return false;
        if (!vertexData || !indexData) return false;
        if (!faceMap) faceMapTexels = 0;

        // If section already exists, remove it first (re-upload)
        if (m_regions.count(key)) {
            RemoveSection(key);
        }

        // Try to fit in an existing slab (last first — most likely to have space)
        for (int i = static_cast<int>(m_slabs.size()) - 1; i >= 0; i--) {
            if (TryUploadToSlab(static_cast<uint32_t>(i), key, vertexData, vertexCount, indexData, indexCount,
                                faceMap, faceMapTexels))
                return true;
        }

        // No slab has space — allocate a new one (<1ms, zero copy)
        uint32_t newSlab = AllocateSlab();
        if (newSlab == UINT32_MAX) return false;
        return TryUploadToSlab(newSlab, key, vertexData, vertexCount, indexData, indexCount,
                               faceMap, faceMapTexels);
    }

    bool ChunkMegaBuffer::AllocSlot(Slab& slab, uint16_t& outSlot) {
        if (!slab.freeSlots.empty()) {
            outSlot = slab.freeSlots.back();
            slab.freeSlots.pop_back();
            return true;
        }
        if (slab.slotHighWater < kSlotsPerSlab) {
            outSlot = static_cast<uint16_t>(slab.slotHighWater++);
            return true;
        }
        return false;   // every origin row taken — the caller tries the next slab
    }

    void ChunkMegaBuffer::FreeSlot(Slab& slab, uint16_t slot) {
        slab.freeSlots.push_back(slot);
    }

    bool ChunkMegaBuffer::TryUploadToSlab(uint32_t slabIndex, const MegaBufferSectionKey& key,
                                           const float* vertexData, size_t vertexCount,
                                           const uint16_t* indexData, size_t indexCount,
                                           const uint32_t* faceMap, size_t faceMapTexels) {
        if (!g_renderBackend) return false;
        Slab& slab = m_slabs[slabIndex];

        // Origin-table row first: it is the scarcer resource in a slab of
        // small sections, and failing here costs nothing to undo.
        uint16_t slot = 0;
        if (!AllocSlot(slab, slot)) return false;

        // Vertex allocation: the vertices plus the face-map records behind
        // them, in whole vertex-stride units (see Region::allocUnits).
        const size_t faceMapUnits = (faceMapTexels + kWordsPerUnit - 1) / kWordsPerUnit;   // faceMapTexels = uint32 words
        const size_t allocUnits = vertexCount + faceMapUnits;
        size_t vertexOffset = 0;
        if (!AllocRegion(slab.freeVertexBlocks, slab.vertexHighWater, slab.vboCapacity, allocUnits, vertexOffset)) {
            FreeSlot(slab, slot);
            return false;
        }

        // Index allocation. In per-section mode the slab IBO is untouched — the
        // section gets its own buffer below — so there is nothing to reserve and
        // nothing to fail on.
        size_t indexOffset = 0;
        if (!m_perSectionIndexBuffers) {
            if (!AllocRegion(slab.freeIndexBlocks, slab.indexHighWater, slab.iboCapacity, indexCount, indexOffset)) {
                // Undo vertex allocation
                FreeRegion(slab.freeVertexBlocks, vertexOffset, allocUnits);
                FreeSlot(slab, slot);
                return false;
            }
        }

        // The section's origin, one vec4 row the vertex shader adds back to
        // every vertex's section-relative position. Written before the
        // vertices that reference it. The row is either brand new or was
        // retired kFreeDelayFrames ago, so no in-flight frame reads it.
        if (m_originsUbo != INVALID_BUFFER) {
            const glm::vec4 origin(static_cast<float>(key.chunkPos.x * 16),
                                   static_cast<float>(Config::MinY + key.sectionY * 16),
                                   static_cast<float>(key.chunkPos.z * 16), 0.0f);
            g_renderBackend->UpdateBufferUnsynchronized(m_originsUbo,
                                          slabIndex * kSlotBytes + slot * kOriginEntryBytes,
                                          kOriginEntryBytes, &origin);
        }

        // Patch the row number into every vertex (the mesher leaves it 0 and
        // only the flags set) and, for face-mapped vertices, add the slab
        // texel position of this section's records to their record index.
        // Then one upload of vertices + records. A memcpy plus one or two
        // stores per vertex, on the render thread, against the upload it
        // precedes.
        {
            const size_t vertexWords = vertexCount * (VERTEX_STRIDE / sizeof(uint32_t));
            const size_t words = (vertexCount + faceMapUnits) * (VERTEX_STRIDE / sizeof(uint32_t));
            m_vertexScratch.resize(words);
            std::memcpy(m_vertexScratch.data(), vertexData, vertexCount * VERTEX_STRIDE);
            const uint32_t recordBase = static_cast<uint32_t>((vertexOffset + vertexCount) * kRecordsPerUnit);
            auto* verts = reinterpret_cast<TerrainVertex*>(m_vertexScratch.data());
            for (size_t i = 0; i < vertexCount; ++i) {
                verts[i].slot = static_cast<uint16_t>((verts[i].slot & TerrainVertex::kFlagMask) | slot);
                if (verts[i].slot & TerrainVertex::kMapFlag) verts[i].packedColor += recordBase;
            }
            if (faceMapTexels > 0) {
                std::memcpy(m_vertexScratch.data() + vertexWords, faceMap, faceMapTexels * sizeof(uint32_t));
                // Pad the last unit so no stale scratch reaches the GPU.
                std::fill(m_vertexScratch.begin() + static_cast<std::ptrdiff_t>(vertexWords + faceMapTexels),
                          m_vertexScratch.end(), 0u);
            }
        }
        // Unsynchronised on purpose, on both backends. Every range written
        // here is either past the high-water mark or was retired
        // kFreeDelayFrames ago (RetireFreedRegions), so no in-flight frame
        // reads it — and on OpenGL the synchronised glBufferSubData did not
        // know that: Apple's driver serialised it against every pending draw
        // of the slab, a GPU-length stall per upload (MeshUpload p99 6-12 ms
        // on tour1, 2026-09-04). The one reader that could still reach a
        // fresh range is a bridged gap (see m_recentIndexAllocs).
        g_renderBackend->UpdateBufferUnsynchronized(slab.vbo,
                                       vertexOffset * VERTEX_STRIDE,
                                       allocUnits * VERTEX_STRIDE,
                                       m_vertexScratch.data());

        // Upload index data, rebased to absolute so the draw needs no
        // baseVertex (see INDEX_SIZE). Both modes get the same layout so the
        // renderer never has to know which one it is drawing from.
        ConvertToAbsolute(m_indexScratch, indexData, indexCount, vertexOffset);
        BufferHandle sectionIbo = INVALID_BUFFER;
        if (m_perSectionIndexBuffers) {
            sectionIbo = g_renderBackend->CreateBuffer(
                BufferUsage::Index,
                indexCount * INDEX_SIZE,
                m_indexScratch.data(),
                BufferAccess::Dynamic);
            if (sectionIbo == INVALID_BUFFER) {
                FreeRegion(slab.freeVertexBlocks, vertexOffset, allocUnits);
                FreeSlot(slab, slot);
                return false;
            }
        } else {
            g_renderBackend->UpdateBufferUnsynchronized(slab.ibo,
                                           indexOffset * INDEX_SIZE,
                                           indexCount * INDEX_SIZE,
                                           m_indexScratch.data());
            m_recentIndexAllocs.push_back({slabIndex, indexOffset, indexCount, m_frameCounter});
            slab.hotRangesDirty = true;
        }

        m_uploadedBytes += allocUnits * VERTEX_STRIDE + indexCount * INDEX_SIZE;

        // Store region
        Region region{};
        region.slabIndex    = slabIndex;
        region.vertexOffset = vertexOffset;
        region.vertexCount  = vertexCount;
        region.indexOffset  = indexOffset;
        region.indexCount   = indexCount;
        region.slot         = slot;
        region.allocUnits   = allocUnits;
        region.sectionIbo   = sectionIbo;
        m_regions[key] = region;
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

        // Same rebase as the upload; the section's vertices never move, so
        // its vertexOffset is the same one the original indices were built on.
        ConvertToAbsolute(m_indexScratch, indexData, indexCount, region.vertexOffset);

        // Per-section mode writes a buffer only this section draws from, so the
        // driver has no in-flight draws to serialise against — that stall is the
        // entire reason this mode exists.
        if (m_perSectionIndexBuffers) {
            if (region.sectionIbo == INVALID_BUFFER) return false;
            g_renderBackend->UpdateBuffer(region.sectionIbo, 0,
                                          indexCount * INDEX_SIZE, m_indexScratch.data());
        } else {
            // Unsynchronised: this is a same-length permutation of the section's
            // own quad range (enforced above), so a torn read is a mix of two
            // valid orderings, never an invalid index — absolute indices keep
            // that property, every word written still lands inside this
            // section's own vertex range. See RenderBackend::UpdateBufferUnsynchronized.
            g_renderBackend->UpdateBufferUnsynchronized(
                m_slabs[region.slabIndex].ibo,
                region.indexOffset * INDEX_SIZE,
                indexCount * INDEX_SIZE,
                m_indexScratch.data());
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
                                      region.vertexOffset, region.allocUnits,
                                      region.indexOffset, region.indexCount,
                                      region.slot,
                                      !m_perSectionIndexBuffers,
                                      m_frameCounter});
            // The parked range still holds live-looking indices until retire
            // zeroes it; the renderer must not bridge a merged draw across it.
            if (!m_perSectionIndexBuffers) slab.hotRangesDirty = true;
            if (slab.sectionCount > 0) slab.sectionCount--;
        }
        m_regions.erase(it);
    }

    bool ChunkMegaBuffer::DebugGetRegionInfo(const MegaBufferSectionKey& key, uint32_t& outSlab,
                                             size_t& outVtxOff, size_t& outVtxCnt,
                                             size_t& outIdxOff, size_t& outIdxCnt) const {
        auto it = m_regions.find(key);
        if (it == m_regions.end()) return false;
        outSlab   = it->second.slabIndex;
        outVtxOff = it->second.vertexOffset;
        outVtxCnt = it->second.vertexCount;
        outIdxOff = it->second.indexOffset;
        outIdxCnt = it->second.indexCount;
        return true;
    }
    BufferHandle ChunkMegaBuffer::DebugGetSlabVbo(uint32_t slab) const {
        return slab < m_slabs.size() ? m_slabs[slab].vbo : INVALID_BUFFER;
    }
    BufferHandle ChunkMegaBuffer::DebugGetSlabIbo(uint32_t slab) const {
        return slab < m_slabs.size() ? m_slabs[slab].ibo : INVALID_BUFFER;
    }

    void ChunkMegaBuffer::RetireFreedRegions() {
        m_frameCounter++;
        // Recent allocations stop being hot once every frame that could have
        // bridged across them has retired.
        if (!m_recentIndexAllocs.empty()) {
            size_t keepA = 0;
            for (const RecentAlloc& a : m_recentIndexAllocs) {
                if (m_frameCounter - a.frameAllocated < kFreeDelayFrames) {
                    m_recentIndexAllocs[keepA++] = a;
                } else if (a.slabIndex < m_slabs.size()) {
                    m_slabs[a.slabIndex].hotRangesDirty = true;
                }
            }
            m_recentIndexAllocs.resize(keepA);
        }
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
                FreeSlot(slab, p.slot);
                if (p.freeIndices) {
                    // Zero the range before it becomes free space. Index 0 is
                    // always a valid vertex (slab vertex 0), so a run of zeros
                    // is a run of zero-area triangles that rasterise nothing —
                    // which is what makes it legal for the renderer to draw
                    // straight across freed gaps when merging sections (see
                    // IsIndexGapDrawable). Written here rather than at
                    // RemoveSection because a frame may still be drawing this
                    // range then, and tearing the old mesh out from under it
                    // is precisely the one-frame hole the delay exists to
                    // prevent. Free-list reuse re-fills whatever gets
                    // allocated; what stays free stays zero.
                    if (m_zeroScratch.size() < p.indexCount) {
                        m_zeroScratch.resize(p.indexCount, 0u);
                    }
                    if (g_renderBackend && slab.ibo != INVALID_BUFFER) {
                        // No draw has touched this range for kFreeDelayFrames
                        // (a live section: freed; a bridged gap: refused
                        // while hot), so the write needs no ordering.
                        g_renderBackend->UpdateBufferUnsynchronized(slab.ibo,
                                                      p.indexOffset * INDEX_SIZE,
                                                      p.indexCount * INDEX_SIZE,
                                                      m_zeroScratch.data());
                        m_uploadedBytes += p.indexCount * INDEX_SIZE;
                    }
                    FreeRegion(slab.freeIndexBlocks, p.indexOffset, p.indexCount);
                    slab.hotRangesDirty = true;
                }
            }
        }
        m_pendingFrees.resize(keep);
    }

    void ChunkMegaBuffer::RefreshHotRanges(Slab& slab, uint32_t slabIndex) const {
        slab.hotIndexRanges.clear();
        for (const PendingFree& p : m_pendingFrees) {
            if (p.slabIndex == slabIndex && p.freeIndices) {
                slab.hotIndexRanges.push_back({p.indexOffset, p.indexCount});
            }
        }
        for (const RecentAlloc& a : m_recentIndexAllocs) {
            if (a.slabIndex == slabIndex) {
                slab.hotIndexRanges.push_back({a.indexOffset, a.indexCount});
            }
        }
        std::sort(slab.hotIndexRanges.begin(), slab.hotIndexRanges.end(),
                  [](const Slab::FreeBlock& a, const Slab::FreeBlock& b) { return a.offset < b.offset; });
        slab.hotRangesDirty = false;
    }

    bool ChunkMegaBuffer::IsIndexGapDrawable(uint32_t slabIndex, size_t gapBegin, size_t gapEnd) const {
        if (gapEnd <= gapBegin) return true;               // no gap at all
        if (slabIndex >= m_slabs.size()) return false;
        Slab& slab = m_slabs[slabIndex];
        if (slab.hotRangesDirty) RefreshHotRanges(slab, slabIndex);
        if (slab.hotIndexRanges.empty()) return true;

        // Ranges are disjoint (distinct regions) and sorted by offset, so the
        // only one that can reach into [gapBegin, gapEnd) is the last one
        // starting before gapEnd.
        auto it = std::upper_bound(slab.hotIndexRanges.begin(), slab.hotIndexRanges.end(), gapEnd,
                                   [](size_t end, const Slab::FreeBlock& b) { return end <= b.offset; });
        if (it == slab.hotIndexRanges.begin()) return true;
        --it;
        return it->offset + it->size <= gapBegin;
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
        // Per-section IBO mode keeps its indices at offset 0 of its own
        // buffer; shared mode addresses the slab IBO.
        outCmd.indexOffset = r.sectionIbo != INVALID_BUFFER ? 0u : static_cast<uint32_t>(r.indexOffset);
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
                if (slab.faceMapTex != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(slab.faceMapTex);
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
