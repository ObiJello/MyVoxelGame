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
                                     bool perSectionIndexBuffers, const char* name) {
        if (!m_slabs.empty()) {
            Log::Warning("ChunkMegaBuffer::Initialize called on already-initialized buffer, shutting down first");
            Shutdown();
        }

        m_name = name ? name : "terrain";
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
            g_renderBackend->SetDebugLabel(DebugLabelKind::Buffer, m_originsUbo, "Terrain section origins");
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
            auto destroySlab = [&](Slab& slab) {
                // The buffer texture views the VBO: it goes first.
                if (slab.faceMapTex != INVALID_TEXTURE) g_renderBackend->DestroyTexture(slab.faceMapTex);
                if (slab.vbo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.vbo);
                if (slab.ibo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(slab.ibo);
            };
            for (auto& slab : m_slabs) destroySlab(slab);
            if (m_spareSlab) destroySlab(*m_spareSlab);
            if (m_originsUbo != INVALID_BUFFER) g_renderBackend->DestroyBuffer(m_originsUbo);
        }
        m_originsUbo = INVALID_BUFFER;
        m_spareSlab.reset();
        m_slabs.clear();
        m_regions.clear();
        m_pendingFrees.clear();
        m_recentIndexAllocs.clear();
        m_drainSlab = UINT32_MAX;
        m_drainLastCount = 0;
        m_drainStalls = 0;
        m_drainCooldownUntil = 0;
        m_releasedSinceLog = 0;
        m_recreatedSinceLog = 0;
        m_usedVertexUnits = 0;
        m_usedIndices = 0;
        m_uploadFailed = false;
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

        // A released hole first, lowest index first: its slot (and its rows
        // of the origin table) are already there, and a low slab is where
        // the first-fit search wants the next sections anyway. Nothing can
        // name a hole — it was empty when released — so re-creating it
        // under the same index is invisible to every draw path.
        for (uint32_t i = 0; i < m_slabs.size(); ++i) {
            Slab& hole = m_slabs[i];
            if (!hole.released) continue;
            hole = Slab{};
            CreateSlabResources(hole);
            ++m_recreatedSinceLog;
            // A slab had to come back while one was being drained: the
            // pool is fuller than the drain assumed — stop moving sections
            // around before they ping-pong between slabs.
            if (m_drainSlab != UINT32_MAX) EndDrain(/*cooldown=*/true);
            Log::Debug("ChunkMegaBuffer[%s]: re-created released slab %u (%u live, %u released)",
                       m_name, i, GetLiveSlabCount(), GetReleasedSlabCount());
            return i;
        }

        if (m_slabs.size() >= kMaxSlabs) {
            // The origin-table buffer is sized for kMaxSlabs. Reached when
            // parked meshes (the chunk retention cache) and live ones add up
            // past it — a far /tp parks a whole render distance of terrain.
            // The caller re-dirties the section and the chunk manager frees
            // parked meshes (ConsumeUploadFailed); one line per interval.
            m_uploadFailed = true;
            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastLimitLog >= std::chrono::seconds(5)) {
                Log::Error("ChunkMegaBuffer: slab limit (%u) reached; section not uploaded "
                           "(%u more since the last report) — dropping parked meshes to make room",
                           kMaxSlabs, m_suppressedLimitLogs);
                m_lastLimitLog = now;
                m_suppressedLimitLogs = 0;
            } else {
                ++m_suppressedLimitLogs;
            }
            return UINT32_MAX;
        }

        Slab slab;
        const bool fromSpare = m_spareSlab.has_value();
        if (fromSpare) {
            slab = std::move(*m_spareSlab);
            m_spareSlab.reset();
        } else {
            CreateSlabResources(slab);
        }
        if (m_drainSlab != UINT32_MAX) EndDrain(/*cooldown=*/true);   // as for a re-created hole

        uint32_t index = static_cast<uint32_t>(m_slabs.size());
        m_slabs.push_back(std::move(slab));

        Log::Debug("ChunkMegaBuffer[%s]: allocated slab %u (%.1f MB VBO + %.1f MB IBO)%s",
                   m_name, index,
                   static_cast<double>(m_slabVertexCapacity * VERTEX_STRIDE) / (1024.0 * 1024.0),
                   static_cast<double>(m_slabIndexCapacity * INDEX_SIZE) / (1024.0 * 1024.0),
                   fromSpare ? " from the spare" : "");
        PrefetchSpareSlab();
        return index;
    }

    void ChunkMegaBuffer::PrefetchSpareSlab() {
        if (m_spareSlab || !g_renderBackend || !g_renderBackend->PrefersBufferPrefetch()) return;
        if (m_slabs.size() < 2 || m_slabs.size() >= kMaxSlabs) return;   // a growing pool, below its cap
        Slab spare;
        CreateSlabResources(spare);
        m_spareSlab = std::move(spare);
    }

    void ChunkMegaBuffer::CreateSlabResources(Slab& slab) {
        slab.vboCapacity = m_slabVertexCapacity;
        slab.iboCapacity = m_slabIndexCapacity;
        slab.released = false;

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
        g_renderBackend->SetDebugLabel(DebugLabelKind::Buffer, slab.vbo, "Terrain slab VBO");
        g_renderBackend->SetDebugLabel(DebugLabelKind::Buffer, slab.ibo, "Terrain slab IBO");

        // The face map: the same bytes as the VBO, seen by the fragment
        // shader as RGBA16 texels (one record each). One view per slab,
        // bound with the slab.
        // Integer texels: the records are uint16 fields, and the fragment
        // shader reads them as such (usamplerBuffer) — the unorm view cost
        // it a multiply-round-convert per fetch (Xcode per-line, 2026-10-07).
        slab.faceMapTex = g_renderBackend->CreateBufferTexture(slab.vbo, TextureFormat::RGBA16UI);
        g_renderBackend->SetDebugLabel(DebugLabelKind::Texture, slab.faceMapTex, "Terrain face map");
        if (slab.faceMapTex == INVALID_TEXTURE) {
            Log::Error("ChunkMegaBuffer: no buffer texture for the face map — "
                       "greedy-merged rectangles will read garbage records");
        }
    }

    void ChunkMegaBuffer::ReleaseSlab(uint32_t slabIndex) {
        if (slabIndex >= m_slabs.size()) return;
        Slab& slab = m_slabs[slabIndex];
        if (slab.released || slab.sectionCount != 0) return;
        if (g_renderBackend) {
            // Deferred: the slab emptied this frame, but the PREVIOUS
            // frame's command stream may still reference these buffers.
            // GL's default is an immediate delete (driver refcounts
            // pending commands); Vulkan queues the handles behind the
            // fence of the last submitted frame (this runs before
            // BeginFrame, see VKBackend::DeletionSlot) — destroying
            // immediately there is use-after-free. The buffer texture
            // views the VBO: it goes first.
            if (slab.faceMapTex != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(slab.faceMapTex);
            if (slab.vbo != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slab.vbo);
            if (slab.ibo != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slab.ibo);
        }
        // Everything the slab knew goes with its buffers: free-lists, slots,
        // hot and fenced ranges. Capacities read 0, so the statistics and
        // the first-fit search pass over it.
        slab = Slab{};
        slab.released = true;

        // Ranges parked for this slab must not reach RetireFreedRegions: it
        // would zero a range of a destroyed IBO now, or — once the hole is
        // re-created — hand a stale range to the NEW slab's free-list as
        // free space that may be in use. Same for the hot-range bookkeeping.
        m_pendingFrees.erase(
            std::remove_if(m_pendingFrees.begin(), m_pendingFrees.end(),
                           [slabIndex](const PendingFree& p) { return p.slabIndex == slabIndex; }),
            m_pendingFrees.end());
        m_recentIndexAllocs.erase(
            std::remove_if(m_recentIndexAllocs.begin(), m_recentIndexAllocs.end(),
                           [slabIndex](const RecentAlloc& a) { return a.slabIndex == slabIndex; }),
            m_recentIndexAllocs.end());

        if (m_drainSlab == slabIndex) EndDrain(/*cooldown=*/false);   // drained: done
        ++m_releasedSinceLog;
        Log::Debug("ChunkMegaBuffer[%s]: released empty slab %u", m_name, slabIndex);
    }

    uint32_t ChunkMegaBuffer::GetLiveSlabCount() const {
        uint32_t live = 0;
        for (const Slab& slab : m_slabs) live += slab.released ? 0u : 1u;
        return live;
    }

    void ChunkMegaBuffer::ReportSlabChurn() {
        if (m_releasedSinceLog == 0 && m_recreatedSinceLog == 0) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastChurnLog < std::chrono::seconds(10)) return;
        m_lastChurnLog = now;
        const double slabMB = static_cast<double>(m_slabVertexCapacity * VERTEX_STRIDE +
                                                  m_slabIndexCapacity * INDEX_SIZE) / (1024.0 * 1024.0);
        const uint32_t live = GetLiveSlabCount();
        Log::Info("ChunkMegaBuffer[%s]: released %u / re-created %u slabs since the last report; "
                  "%u live (%.0f MB), %u released holes",
                  m_name, m_releasedSinceLog, m_recreatedSinceLog,
                  live, live * slabMB, GetSlabCount() - live);
        m_releasedSinceLog = 0;
        m_recreatedSinceLog = 0;
    }

    // ========================================================================
    // SLAB BINDING
    // ========================================================================

    void ChunkMegaBuffer::BindSlab(uint32_t slabIndex) const {
        if (slabIndex >= m_slabs.size() || !g_renderBackend) return;
        const Slab& slab = m_slabs[slabIndex];
        if (slab.released) return;   // never drawn: a hole holds no section

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
                                         const uint32_t* faceMap, size_t faceMapTexels,
                                       int32_t fadeStartMs) {
        PROFILE_ZONE;
        if (vertexCount == 0 || indexCount == 0) return false;
        if (!vertexData || !indexData) return false;
        if (!faceMap) faceMapTexels = 0;

        // If section already exists, remove it first (re-upload)
        if (m_regions.count(key)) {
            RemoveSection(key);
        }

        // First fit by slab INDEX: the lowest slab with room. Live geometry
        // then packs into the low slabs and the high ones drain on their own
        // as sections are re-meshed or unloaded, which is what lets
        // CompactIfNeeded give their memory back (the old last-slab-first
        // order spread a far teleport's churn over every slab for good).
        // Full slabs are skipped in O(1) by their minFailed hints. The
        // slab being drained (CollectRelocationCandidates) is the last
        // resort before a new slab: a section moved out must not land back.
        const uint32_t slabCount = static_cast<uint32_t>(m_slabs.size());
        for (uint32_t i = 0; i < slabCount; ++i) {
            if (m_slabs[i].released || i == m_drainSlab) continue;
            if (TryUploadToSlab(i, key, vertexData, vertexCount, indexData, indexCount,
                                faceMap, faceMapTexels, fadeStartMs))
                return true;
        }
        if (m_drainSlab < slabCount && !m_slabs[m_drainSlab].released &&
            TryUploadToSlab(m_drainSlab, key, vertexData, vertexCount, indexData, indexCount,
                            faceMap, faceMapTexels, fadeStartMs)) {
            return true;
        }

        // No slab has space — re-create a released one or allocate a new one
        // (<1ms, zero copy)
        uint32_t newSlab = AllocateSlab();
        if (newSlab == UINT32_MAX) return false;
        return TryUploadToSlab(newSlab, key, vertexData, vertexCount, indexData, indexCount,
                               faceMap, faceMapTexels, fadeStartMs);
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
                                           const uint32_t* faceMap, size_t faceMapTexels,
                                           int32_t fadeStartMs) {
        if (!g_renderBackend) return false;
        Slab& slab = m_slabs[slabIndex];
        if (slab.released) return false;

        // Vertex allocation: the vertices plus the face-map records behind
        // them, in whole vertex-stride units (see Region::allocUnits). The
        // records must start on an 8-byte texel; a 20-byte vertex run ends
        // on a 4-byte boundary, so a face-mapped region reserves one spare
        // unit for the (0 or 4 byte) pad.
        const size_t faceMapUnits = faceMapTexels > 0
            ? (faceMapTexels + kWordsPerUnit - 1) / kWordsPerUnit + 1       // faceMapTexels = uint32 words
            : 0;
        const size_t allocUnits = vertexCount + faceMapUnits;

        // Known full for a request this size (see Slab::minFailedUnits):
        // skip without touching the free-lists.
        if (allocUnits >= slab.minFailedUnits) return false;
        if (!m_perSectionIndexBuffers && indexCount >= slab.minFailedIndices) return false;

        // Origin-table row first: it is the scarcer resource in a slab of
        // small sections, and failing here costs nothing to undo.
        uint16_t slot = 0;
        if (!AllocSlot(slab, slot)) return false;

        size_t vertexOffset = 0;
        if (!AllocRegion(slab.freeVertexBlocks, slab.vertexHighWater, slab.vboCapacity, allocUnits, vertexOffset)) {
            slab.minFailedUnits = std::min(slab.minFailedUnits, allocUnits);
            FreeSlot(slab, slot);
            return false;
        }

        // Index allocation. In per-section mode the slab IBO is untouched — the
        // section gets its own buffer below — so there is nothing to reserve and
        // nothing to fail on.
        size_t indexOffset = 0;
        if (!m_perSectionIndexBuffers) {
            if (!AllocRegion(slab.freeIndexBlocks, slab.indexHighWater, slab.iboCapacity, indexCount, indexOffset)) {
                slab.minFailedIndices = std::min(slab.minFailedIndices, indexCount);
                // Undo vertex allocation (never more room than before the
                // attempt, so the minFailed hints stay true)
                FreeRegion(slab.freeVertexBlocks, vertexOffset, allocUnits);
                FreeSlot(slab, slot);
                return false;
            }
        }

        // The section's origin, one ivec4 row the vertex shader adds back to
        // every vertex's section-relative position — after subtracting the
        // view's render origin from it in integer arithmetic (camera-
        // relative rendering, RenderOrigin.hpp). Written before the
        // vertices that reference it. The row is either brand new or was
        // retired kFreeDelayFrames ago, so no in-flight frame reads it.
        if (m_originsUbo != INVALID_BUFFER) {
            // .w: the section's first-upload time, the fade-in's start
            // (SectionFade.hpp); the shader compares it with uFadeNowMs.
            const glm::ivec4 origin(key.chunkPos.x * 16,
                                    Config::MinY + key.sectionY * 16,
                                    key.chunkPos.z * 16, fadeStartMs);
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
            const size_t words = allocUnits * (VERTEX_STRIDE / sizeof(uint32_t));
            m_vertexScratch.assign(words, 0u);          // pads and spare unit zeroed
            std::memcpy(m_vertexScratch.data(), vertexData, vertexCount * VERTEX_STRIDE);
            // First record texel: the slab byte just past the vertices,
            // rounded up to a texel.
            const size_t vertexEndByte = (vertexOffset + vertexCount) * VERTEX_STRIDE;
            const size_t recordByte = (vertexEndByte + kRecordBytes - 1) / kRecordBytes * kRecordBytes;
            const uint32_t recordBase = static_cast<uint32_t>(recordByte / kRecordBytes);
            auto* verts = reinterpret_cast<TerrainVertex*>(m_vertexScratch.data());
            for (size_t i = 0; i < vertexCount; ++i) {
                verts[i].slot = static_cast<uint16_t>(
                    (verts[i].slot & (TerrainVertex::kFlagMask | TerrainVertex::kNormalMask)) | slot);
                if (verts[i].slot & TerrainVertex::kMapFlag) verts[i].packedColor += recordBase;
            }
            if (faceMapTexels > 0) {
                const size_t recordWord = vertexWords + (recordByte - vertexEndByte) / sizeof(uint32_t);
                std::memcpy(m_vertexScratch.data() + recordWord, faceMap, faceMapTexels * sizeof(uint32_t));
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
        slab.usedUnits += allocUnits;
        slab.usedIndices += m_perSectionIndexBuffers ? 0 : indexCount;
        m_usedVertexUnits += allocUnits;
        m_usedIndices += m_perSectionIndexBuffers ? 0 : indexCount;
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
        m_usedVertexUnits -= std::min(m_usedVertexUnits, region.allocUnits);
        if (region.sectionIbo == INVALID_BUFFER) m_usedIndices -= std::min(m_usedIndices, region.indexCount);
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
            if (region.noBridge != 0) slab.parkedIndexRanges.erase(region.indexOffset);
            if ((region.noBridge & kNoBridgeParked) && slab.parkedCount > 0) slab.parkedCount--;
            if (slab.sectionCount > 0) slab.sectionCount--;
            slab.usedUnits -= std::min(slab.usedUnits, region.allocUnits);
            if (region.sectionIbo == INVALID_BUFFER) slab.usedIndices -= std::min(slab.usedIndices, region.indexCount);
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
        return slab < m_slabs.size() ? m_slabs[slab].vbo : INVALID_BUFFER;   // INVALID for a released hole
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
            if (p.slabIndex < m_slabs.size() && !m_slabs[p.slabIndex].released) {
                Slab& slab = m_slabs[p.slabIndex];
                FreeRegion(slab.freeVertexBlocks, p.vertexOffset, p.vertexCount);
                FreeSlot(slab, p.slot);
                // Room came back: every "too big for this slab" verdict is
                // void (Slab::minFailedUnits).
                slab.minFailedUnits = SIZE_MAX;
                slab.minFailedIndices = SIZE_MAX;
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

    void ChunkMegaBuffer::SetSectionNoBridge(const MegaBufferSectionKey& key, uint8_t reason, bool on) {
        auto it = m_regions.find(key);
        if (it == m_regions.end()) return;
        Region& region = it->second;
        const uint8_t before = region.noBridge;
        region.noBridge = on ? static_cast<uint8_t>(before | reason) : static_cast<uint8_t>(before & ~reason);
        // Parked regions pin their slab (it cannot be drained by re-meshing).
        if (((before ^ region.noBridge) & kNoBridgeParked) && region.slabIndex < m_slabs.size()) {
            uint32_t& parked = m_slabs[region.slabIndex].parkedCount;
            if (region.noBridge & kNoBridgeParked) ++parked;
            else if (parked > 0) --parked;
        }
        if ((before != 0) == (region.noBridge != 0)) return;   // fenced-ness unchanged
        if (region.slabIndex >= m_slabs.size() || region.indexCount == 0 ||
            region.sectionIbo != INVALID_BUFFER) {
            return;   // per-section IBOs are never bridged
        }
        auto& ranges = m_slabs[region.slabIndex].parkedIndexRanges;
        if (region.noBridge != 0) ranges[region.indexOffset] = region.indexCount;
        else                      ranges.erase(region.indexOffset);
    }

    std::vector<std::vector<ChunkMegaBuffer::DebugRegionRef>> ChunkMegaBuffer::DebugRegionsBySlab() const {
        std::vector<std::vector<DebugRegionRef>> out(m_slabs.size());
        for (const auto& [key, region] : m_regions) {
            if (region.slabIndex >= out.size() || region.indexCount == 0) continue;
            out[region.slabIndex].push_back({region.indexOffset, region.indexCount, key});
        }
        for (auto& slab : out) {
            std::sort(slab.begin(), slab.end(),
                      [](const DebugRegionRef& a, const DebugRegionRef& b) { return a.indexOffset < b.indexOffset; });
        }
        return out;
    }

    bool ChunkMegaBuffer::IsIndexGapDrawable(uint32_t slabIndex, size_t gapBegin, size_t gapEnd) const {
        if (gapEnd <= gapBegin) return true;               // no gap at all
        if (slabIndex >= m_slabs.size()) return false;
        Slab& slab = m_slabs[slabIndex];
        // A fenced section in the gap (SetSectionNoBridge): terrain of an
        // unloaded chunk, or of one outside the rendered view — never to be
        // drawn. Same disjoint-and-sorted argument as the hot ranges below.
        if (!slab.parkedIndexRanges.empty()) {
            auto p = slab.parkedIndexRanges.lower_bound(gapEnd);   // first starting at/after the end
            if (p != slab.parkedIndexRanges.begin()) {
                --p;
                if (p->first + p->second > gapBegin) return false;
            }
        }
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

    bool ChunkMegaBuffer::GetSectionVertexOffset(const MegaBufferSectionKey& key, size_t& outVertexOffset) const {
        auto it = m_regions.find(key);
        if (it == m_regions.end()) return false;
        outVertexOffset = it->second.vertexOffset;
        return true;
    }

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
        // With slab pool, "compaction" is releasing empty slabs. We never
        // copy data between slabs — free-list reuse handles fragmentation
        // inside one, and re-meshing (CollectRelocationCandidates) empties
        // the thinly used ones.
        //
        // Any empty slab may go, not only trailing ones: it is RELEASED
        // (buffers destroyed, slot kept), so the index of every other slab —
        // what cached draw commands and the origin table address — never
        // changes. Releasing is safe for the draw paths because an empty
        // slab has no region: GPUSectionData draw commands, merged/bridged
        // runs (IsIndexGapDrawable works within a slab, between live
        // regions) and per-section IBOs all come from live regions, and
        // the draw list is rebuilt from them every frame. Ranges freed in
        // the last frames still sit in m_pendingFrees and are purged by
        // ReleaseSlab; the frame in flight is covered by the deferred
        // destroy.
        //
        // Only a QUIESCENT empty slab goes: none of its ranges still waits
        // in m_pendingFrees, i.e. nothing in it was drawn for the last
        // kFreeDelayFrames frames. A slab emptied a moment ago may still be
        // read by the frame in flight — not just its buffers (the deferred
        // destroy covers those) but its rows of the shared origin table,
        // which a hole re-created this same frame would overwrite with
        // unsynchronised writes under that frame's feet.
        //
        // Kept: slab 0 (IsInitialized, BindSlab always have one) and ONE
        // empty spare, the lowest-index empty slab — the next uploads fill
        // it first, and a pool that just emptied a slab while streaming
        // would otherwise release and re-create one every compaction.
        m_compactBusy.assign(m_slabs.size(), 0);
        for (const PendingFree& p : m_pendingFrees) {
            if (p.slabIndex < m_compactBusy.size()) m_compactBusy[p.slabIndex] = 1;
        }
        bool released = false;
        bool keptSpare = false;
        for (uint32_t i = 0; i < m_slabs.size(); ++i) {
            const Slab& slab = m_slabs[i];
            if (slab.released || slab.sectionCount != 0) continue;
            if (i == 0 || !keptSpare) { keptSpare = true; continue; }
            if (m_compactBusy[i]) continue;   // released at a later compaction
            ReleaseSlab(i);
            released = true;
        }

        // Trailing holes carry nothing a re-creation needs (their origin
        // rows are rewritten per slot as sections arrive), so the vector is
        // trimmed back as the old tail-only path did. Every entry for them
        // was purged by ReleaseSlab.
        while (m_slabs.size() > 1 && m_slabs.back().released) {
            m_slabs.pop_back();
        }
        if (m_drainSlab != UINT32_MAX && m_drainSlab >= m_slabs.size()) EndDrain(/*cooldown=*/false);

        ReportSlabChurn();
        return released;
    }

    // ========================================================================
    // CONSOLIDATION
    // ========================================================================

    void ChunkMegaBuffer::EndDrain(bool cooldown) {
        if (m_drainSlab == UINT32_MAX) return;
        if (cooldown) {
            m_drainCooldownUntil = m_frameCounter + kDrainCooldownFrames;
            Log::Debug("ChunkMegaBuffer[%s]: gave up draining slab %u (%zu sections left)",
                       m_name, m_drainSlab,
                       m_drainSlab < m_slabs.size() ? m_slabs[m_drainSlab].sectionCount : size_t(0));
        }
        m_drainSlab = UINT32_MAX;
        m_drainLastCount = 0;
        m_drainStalls = 0;
    }

    uint32_t ChunkMegaBuffer::PickDrainSlab() const {
        const uint32_t slabCount = static_cast<uint32_t>(m_slabs.size());
        uint32_t live = 0;
        size_t capUnits = 0, capIndices = 0;
        for (const Slab& s : m_slabs) {
            if (s.released) continue;
            ++live;
            capUnits += s.vboCapacity;
            capIndices += s.iboCapacity;
        }
        if (live < 2 || capUnits == 0) return UINT32_MAX;

        // Mostly empty, by every resource a slab can run out of.
        const auto share = [](size_t used, size_t cap) {
            return cap > 0 ? static_cast<float>(used) / static_cast<float>(cap) : 0.0f;
        };
        float utilization = std::max(share(m_usedVertexUnits, capUnits),
                                     share(m_regions.size(), size_t(live) * kSlotsPerSlab));
        if (!m_perSectionIndexBuffers) utilization = std::max(utilization, share(m_usedIndices, capIndices));
        if (utilization >= kDefragUtilization) return UINT32_MAX;

        // Room a section can actually land in, per slab: the untouched tail
        // plus free blocks big enough for a typical section layer. Ranges
        // still in m_pendingFrees are not counted (conservative).
        std::vector<size_t> roomUnits(slabCount, 0), roomIndices(slabCount, 0), roomSlots(slabCount, 0);
        size_t totalUnits = 0, totalIndices = 0, totalSlots = 0;
        for (uint32_t i = 0; i < slabCount; ++i) {
            const Slab& s = m_slabs[i];
            if (s.released) continue;
            size_t units = s.vboCapacity - std::min(s.vboCapacity, s.vertexHighWater);
            for (const Slab::FreeBlock& b : s.freeVertexBlocks) {
                if (b.size >= kMinUsefulUnits) units += b.size;
            }
            size_t indices = s.iboCapacity - std::min(s.iboCapacity, s.indexHighWater);
            for (const Slab::FreeBlock& b : s.freeIndexBlocks) {
                if (b.size >= kMinUsefulIndices) indices += b.size;
            }
            const size_t slots = kSlotsPerSlab - std::min<size_t>(kSlotsPerSlab, s.sectionCount);
            roomUnits[i] = units;     totalUnits += units;
            roomIndices[i] = indices; totalIndices += indices;
            roomSlots[i] = slots;     totalSlots += slots;
        }

        // The sparsest slab whose sections the others can take; a tie goes
        // to the higher index (first fit drains the top anyway). Slab 0 is
        // never released, so never drained.
        uint32_t best = UINT32_MAX;
        float bestFill = 2.0f;
        for (uint32_t i = 1; i < slabCount; ++i) {
            const Slab& s = m_slabs[i];
            if (s.released || s.sectionCount == 0 || s.parkedCount != 0) continue;
            float fill = std::max(share(s.usedUnits, s.vboCapacity),
                                  share(s.sectionCount, kSlotsPerSlab));
            if (!m_perSectionIndexBuffers) fill = std::max(fill, share(s.usedIndices, s.iboCapacity));
            if (fill > kDefragMaxSlabFill || fill > bestFill) continue;
            const auto fits = [](size_t otherRoom, size_t need) {
                return static_cast<double>(otherRoom) >= static_cast<double>(need) * kDrainRoomMargin;
            };
            if (!fits(totalUnits - roomUnits[i], s.usedUnits)) continue;
            if (!fits(totalSlots - roomSlots[i], s.sectionCount)) continue;
            if (!m_perSectionIndexBuffers && !fits(totalIndices - roomIndices[i], s.usedIndices)) continue;
            best = i;   // ascending index: an equal fill later replaces it
            bestFill = fill;
        }
        return best;
    }

    bool ChunkMegaBuffer::CollectRelocationCandidates(std::vector<MegaBufferSectionKey>& out, size_t maxCount) {
        out.clear();
        if (maxCount == 0) return false;

        // Progress check on the drain in hand: the caller only asks again
        // once its previous batch has landed, so a section count that did
        // not fall means the re-meshes went back where they came from (or
        // never ran) — give up rather than re-mesh the same sections forever.
        if (m_drainSlab != UINT32_MAX) {
            const Slab* s = m_drainSlab < m_slabs.size() ? &m_slabs[m_drainSlab] : nullptr;
            if (!s || s->released || s->sectionCount == 0) {
                EndDrain(/*cooldown=*/false);            // emptied: CompactIfNeeded releases it
            } else if (s->parkedCount != 0) {
                EndDrain(/*cooldown=*/true);             // a parked mesh pinned it meanwhile
            } else if (s->sectionCount >= m_drainLastCount) {
                if (++m_drainStalls >= kMaxDrainStalls) EndDrain(/*cooldown=*/true);
            } else {
                m_drainStalls = 0;
            }
        }
        if (m_drainSlab == UINT32_MAX) {
            if (m_frameCounter < m_drainCooldownUntil) return false;
            m_drainSlab = PickDrainSlab();
            if (m_drainSlab == UINT32_MAX) return false;
            m_drainStalls = 0;
            const Slab& s = m_slabs[m_drainSlab];
            Log::Debug("ChunkMegaBuffer[%s]: draining slab %u (%zu sections, %.0f%% of its vertices; "
                       "pool %zu sections over %u live slabs)",
                       m_name, m_drainSlab, s.sectionCount,
                       s.vboCapacity ? 100.0 * static_cast<double>(s.usedUnits) / static_cast<double>(s.vboCapacity) : 0.0,
                       m_regions.size(), GetLiveSlabCount());
        }
        m_drainLastCount = m_slabs[m_drainSlab].sectionCount;

        for (const auto& [key, region] : m_regions) {
            if (region.slabIndex != m_drainSlab || (region.noBridge & kNoBridgeParked)) continue;
            out.push_back(key);
            if (out.size() >= maxCount) break;
        }
        return !out.empty();
    }

    bool ChunkMegaBuffer::IsInDrainSlab(const MegaBufferSectionKey& key) const {
        if (m_drainSlab == UINT32_MAX) return false;
        auto it = m_regions.find(key);
        return it != m_regions.end() && it->second.slabIndex == m_drainSlab;
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
