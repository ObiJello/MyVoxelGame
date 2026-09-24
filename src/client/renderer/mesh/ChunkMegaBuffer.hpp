// File: src/client/renderer/mesh/ChunkMegaBuffer.hpp
#pragma once

#include <chrono>

#include "common/world/math/WorldMath.hpp"
#include "../backend/RenderTypes.hpp"
#include <vector>
#include <map>
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

        // faceMap: the layer's face-map records (RGBA8 texels, see
        // TerrainVertex::Mapped), stored right after the vertices in the same
        // slab region and read by the fragment shader through the slab's
        // buffer texture (BindSlab binds it at texture slot 2). The mega
        // buffer adds the records' slab texel position to every face-mapped
        // vertex's record index at upload, next to the origin-row patch.
        // nullptr / 0 for a layer without merged rectangles.
        // `fadeStartMs`: the section's first-upload time (SectionFade.hpp),
        // written to its origin row's .w so the vertex shader can fade it in.
        bool UploadSection(const MegaBufferSectionKey& key,
                           const float* vertexData, size_t vertexCount,
                           const uint16_t* indexData, size_t indexCount,
                           const uint32_t* faceMap = nullptr, size_t faceMapTexels = 0,
                           int32_t fadeStartMs = 0);

        // Rewrites a section's indices in place, leaving its vertices alone.
        // Used by translucent re-sorting, which reorders quads without
        // regenerating any geometry. `indexCount` must equal what the section
        // was uploaded with — the region is not reallocated — otherwise the
        // call is rejected and the old order stays on the GPU.
        // Input is section-relative uint16 like UploadSection; the absolute
        // conversion happens here.
        bool UpdateSectionIndices(const MegaBufferSectionKey& key,
                                  const uint16_t* indexData, size_t indexCount);

        void RemoveSection(const MegaBufferSectionKey& key);
        bool HasSection(const MegaBufferSectionKey& key) const;

        // ========================================================================
        // DRAW COMMANDS
        // ========================================================================

        // 32-bit ABSOLUTE indices. Sections used to upload their uint16
        // section-relative indices verbatim and draw with a per-section
        // baseVertex, which halved index bandwidth but pinned the draw count
        // at one sub-draw per section per layer (10k+ in a wide view — ~0.5us
        // of CPU each inside glMultiDrawElementsBaseVertex on Apple's GL, and
        // on MoltenVK the indirect command count is what scales QueueSubmit).
        // Absolute indices make every section's draw parameters identical
        // apart from its index range, so adjacent ranges in one slab collapse
        // into one sub-draw — the renderer merges them (ChunkRenderer::
        // SubmitMergedRuns). The doubled index memory is the price; the mesher's
        // uint16 output is unchanged and converted at upload.
        static constexpr size_t INDEX_SIZE = sizeof(uint32_t);

        // Indices in the slab IBO are ABSOLUTE (already include the section's
        // vertexOffset), so every section draws with baseVertex 0 — see
        // INDEX_SIZE. That is what lets the renderer fuse neighbouring
        // sections of one slab into a single sub-draw.
        struct DrawCommand {
            int32_t indexCount;
            uint32_t indexOffset;     // In indices, into the slab's IBO
            uint32_t slabIndex;       // Which slab to bind before drawing
        };

        bool GetDrawCommand(const MegaBufferSectionKey& key, DrawCommand& outCmd) const;

        // DEBUG ONLY (F8 CullDump): raw region bookkeeping + slab buffer
        // handles, so the dump can read the actual GPU-side index/vertex data
        // for a section and prove whether the bytes are alive.
        bool DebugGetRegionInfo(const MegaBufferSectionKey& key, uint32_t& outSlab,
                                size_t& outVtxOff, size_t& outVtxCnt,
                                size_t& outIdxOff, size_t& outIdxCnt) const;
        BufferHandle DebugGetSlabVbo(uint32_t slab) const;
        BufferHandle DebugGetSlabIbo(uint32_t slab) const;

        // Whether a draw may run straight across the index range
        // [gapBegin, gapEnd) of `slabIndex` without any section's command
        // covering it. Everything below a slab's high-water mark is one of:
        // a live section of a loaded chunk (drawing it uninvited is harmless:
        // it is outside the frustum or behind terrain, exactly where it
        // belongs), a range zeroed on retire (degenerate triangles, draws
        // nothing), a range freed within the last kFreeDelayFrames that STILL
        // HOLDS its old indices, or a FENCED section (SetSectionNoBridge) — a
        // chunk the client has unloaded and keeps only for an instant
        // revisit, or a loaded one outside the rendered view.
        // The last two are problems: a freed range would keep drawing a mesh
        // the player just replaced (a broken block lingering for three
        // frames), and a parked one draws terrain that is no longer there —
        // stray sections of chunks the player left, coming and going with the
        // view angle as the visible runs around them change (2026-09-24). So
        // this says no exactly when the gap touches one of them.
        bool IsIndexGapDrawable(uint32_t slabIndex, size_t gapBegin, size_t gapEnd) const;

        // Sections a bridged gap must never cross although their ranges are
        // live, by reason (a mask: a section stays fenced while any is set):
        //  - kNoBridgeParked: its chunk is unloaded and kept only in the
        //    retention cache (revival is a pointer swap), so it must never
        //    be drawn;
        //  - kNoBridgeOutsideView: its chunk is loaded but outside what the
        //    client renders — the halo ring the server sends so edge chunks
        //    have neighbours (MC isInViewDistance buffer 1 vs 2), which MC
        //    never draws.
        static constexpr uint8_t kNoBridgeParked      = 1;
        static constexpr uint8_t kNoBridgeOutsideView = 2;
        void SetSectionNoBridge(const MegaBufferSectionKey& key, uint8_t reason, bool on);

        // Diagnostics (OBEY_STRAY_AUDIT): every live region, per slab, sorted
        // by index offset — what a bridged gap actually contains. O(regions);
        // built once per sampled frame, never on the normal path.
        struct DebugRegionRef {
            size_t indexOffset;
            size_t indexCount;
            MegaBufferSectionKey key;
        };
        std::vector<std::vector<DebugRegionRef>> DebugRegionsBySlab() const;

        // ========================================================================
        // SLAB BINDING
        // ========================================================================

        // Bind a specific slab's VBO and IBO via the render backend.
        void BindSlab(uint32_t slabIndex) const;

        uint32_t GetSlabCount() const { return static_cast<uint32_t>(m_slabs.size()); }

        // ── Capacity pressure ──────────────────────────────────────────────
        // The pool can grow to kMaxSlabs and no further. Past three quarters
        // of that ceiling (vertices, indices or origin rows), or after an
        // upload found no room at all, the chunk manager drops parked meshes
        // (the retention cache) so live sections keep fitting — see
        // ClientChunkManager::RelieveMeshBufferPressure.
        bool NearCapacity() const {
            const size_t maxUnits = size_t(kMaxSlabs) * m_slabVertexCapacity;
            const size_t maxIndices = size_t(kMaxSlabs) * m_slabIndexCapacity;
            const size_t maxRows = size_t(kMaxSlabs) * kSlotsPerSlab;
            return m_usedVertexUnits * 4 > maxUnits * 3 ||
                   (!m_perSectionIndexBuffers && m_usedIndices * 4 > maxIndices * 3) ||
                   m_regions.size() * 4 > maxRows * 3;
        }
        bool ConsumeUploadFailed() {
            const bool failed = m_uploadFailed;
            m_uploadFailed = false;
            return failed;
        }

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
            // RGBA16 buffer texture over `vbo` — how the fragment shader reads
            // the face-map records (one 8-byte texel each) that sit behind
            // each section's vertices.
            TextureHandle faceMapTex = INVALID_TEXTURE;
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

            // Index ranges parked in m_pendingFrees for THIS slab, sorted by
            // offset, for IsIndexGapDrawable. Rebuilt lazily (hotRangesDirty)
            // because RemoveSection can run thousands of times in a frame
            // during a remesh flood and the renderer only reads this a few
            // thousand times per pass.
            std::vector<FreeBlock> hotIndexRanges;
            bool hotRangesDirty = false;

            // Index ranges of fenced sections (SetSectionNoBridge), keyed by
            // offset. Regions are disjoint, so the only one that can reach
            // into a gap is the last one starting before the gap's end.
            std::map<size_t, size_t> parkedIndexRanges;

            // Rows of this slab's section-origin table (see kSlotsPerSlab):
            // bump-allocated, recycled through freeSlots after the same
            // retire delay as the vertex range they describe.
            uint32_t slotHighWater = 0;
            std::vector<uint16_t> freeSlots;
        };

        // mutable: IsIndexGapDrawable is logically const but refreshes the
        // per-slab hot-range cache on demand.
        mutable std::vector<Slab> m_slabs;
        size_t m_slabVertexCapacity = 0;
        size_t m_slabIndexCapacity = 0;
        bool m_perSectionIndexBuffers = false;

        // Accumulated by every slab write; drained once a frame by
        // ConsumeUploadedBytes. Render-thread only, so no atomic needed.
        size_t m_uploadedBytes = 0;
        // Live allocations (regions in m_regions), for NearCapacity. A
        // removed region stops counting at RemoveSection, though its range
        // is only reusable kFreeDelayFrames later.
        size_t m_usedVertexUnits = 0;
        size_t m_usedIndices = 0;
        bool   m_uploadFailed = false;
        // Throttle for the slab-limit error (one line per interval, with a count).
        std::chrono::steady_clock::time_point m_lastLimitLog{};
        uint32_t m_suppressedLimitLogs = 0;

        // Per-section region tracking (which slab + offset)
        struct Region {
            uint32_t slabIndex;
            size_t vertexOffset;
            size_t vertexCount;     // real vertices (stats, debug readback)
            size_t indexOffset;
            size_t indexCount;
            uint16_t slot;          // origin-table row, patched into every vertex
            // Vertex-stride units the region actually occupies: the vertices
            // plus the face-map records behind them (rounded up to whole
            // 16-byte units). This, not vertexCount, is what gets freed.
            size_t allocUnits = 0;
            // Per-section index buffer, MC-style (CompiledSectionMesh ->
            // SectionBuffers.getIndexBuffer()). Only used when the pool was
            // initialised with perSectionIndexBuffers; INVALID_BUFFER otherwise
            // and indices live in the slab IBO at indexOffset as before.
            BufferHandle sectionIbo = INVALID_BUFFER;
            uint8_t noBridge = 0;   // reason mask, see SetSectionNoBridge
        };
        std::unordered_map<MegaBufferSectionKey, Region, MegaBufferSectionKeyHash> m_regions;

        // Ranges released by RemoveSection, held back from the free-lists until
        // no in-flight frame can still be drawing them. See RetireFreedRegions.
        struct PendingFree {
            uint32_t slabIndex;
            size_t   vertexOffset;
            size_t   vertexCount;   // allocation units (Region::allocUnits)
            size_t   indexOffset;
            size_t   indexCount;
            uint16_t slot;          // origin row, reusable once the range is
            bool     freeIndices;   // false in per-section-IBO mode
            uint64_t frameFreed;
        };
        std::vector<PendingFree> m_pendingFrees;
        // Index ranges handed out within the last kFreeDelayFrames. They are
        // hot for IsIndexGapDrawable for the opposite reason to the pending
        // frees: a bridged gap drawn by an in-flight frame may run across
        // free space that THIS frame just allocated and is writing new
        // indices into — with unsynchronised uploads (below) nothing else
        // keeps that draw from reading a half-written section.
        struct RecentAlloc {
            uint32_t slabIndex;
            size_t   indexOffset;
            size_t   indexCount;
            uint64_t frameAllocated;
        };
        std::vector<RecentAlloc> m_recentIndexAllocs;
        uint64_t m_frameCounter = 0;
        // MAX_FRAMES_IN_FLIGHT is 2, so frame N can still be reading what frame
        // N-1 drew. 3 covers that with a frame to spare, and the cost of being
        // generous is a few hundred KB of briefly-unreusable slab space.
        static constexpr uint64_t kFreeDelayFrames = 3;

        // Conversion scratch for TryUploadToSlab/UpdateSectionIndices: the
        // mesher hands us uint16 section-relative indices, the slab wants
        // uint32 absolute. Reused across calls so the steady-state upload
        // path allocates nothing.
        std::vector<uint32_t> m_indexScratch;
        // Upload scratch for TryUploadToSlab: the section's vertices with
        // their origin-table slot patched into TerrainVertex::slot.
        std::vector<uint32_t> m_vertexScratch;
        // Zero block written over retired index ranges. Grows to the largest
        // region ever retired and stays there (a few hundred KB at most —
        // a section layer is capped at 65,536 vertices / 98,304 indices).
        std::vector<uint32_t> m_zeroScratch;

        // Bring `slab`'s hotIndexRanges up to date with m_pendingFrees.
        void RefreshHotRanges(Slab& slab, uint32_t slabIndex) const;

        // Slab management
        // Returns the new slab's index, or UINT32_MAX when kMaxSlabs is reached.
        uint32_t AllocateSlab();
        bool TryUploadToSlab(uint32_t slabIndex, const MegaBufferSectionKey& key,
                             const float* vertexData, size_t vertexCount,
                             const uint16_t* indexData, size_t indexCount,
                             const uint32_t* faceMap, size_t faceMapTexels,
                             int32_t fadeStartMs);

        // Internal allocation (first-fit with bump fallback, per-slab)
        static bool AllocRegion(std::vector<Slab::FreeBlock>& freeList, size_t& highWater,
                                size_t capacity, size_t count, size_t& outOffset);
        static void FreeRegion(std::vector<Slab::FreeBlock>& freeList,
                               size_t offset, size_t count);

        // Bytes per terrain vertex — must equal sizeof(Render::TerrainVertex)
        // and GetTerrainVertexLayout().stride: the packed 20-byte vertex.
        static constexpr size_t VERTEX_STRIDE = 20;
        // Face-map records are RGBA16 texels (8 bytes) of the slab's buffer
        // texture; the mesher's record arrays are uint32 words. A vertex unit
        // is 20 bytes, so a region's record array starts at the next 8-byte
        // boundary after its vertices — at most 4 bytes of padding, which one
        // spare unit per face-mapped region always covers.
        static constexpr size_t kRecordBytes  = 8;
        static constexpr size_t kWordsPerUnit = VERTEX_STRIDE / 4;
        static_assert(VERTEX_STRIDE % 4 == 0, "a vertex unit must hold whole record words");

        // --- Section-origin table ------------------------------------------
        // TerrainVertex positions are relative to their section's origin and
        // carry only a 15-bit row number; the origin itself lives here, one
        // vec4 per row, in a uniform buffer the terrain vertex shader indexes
        // (`SectionOrigins`). Merged multi-draws span many sections in one
        // draw, so a per-draw uniform cannot carry it — a per-vertex row can.
        //
        // One table per SLAB, kSlotsPerSlab rows = 16 KB, the uniform-range
        // size every Vulkan device guarantees, bound with BindSlab via the
        // backend's single user uniform block. All slabs' tables sit in one
        // buffer at slab * kSlotBytes, so binding is an offset, never a
        // descriptor change. A slab therefore holds at most kSlotsPerSlab
        // sections; TryUploadToSlab moves on to the next slab when the rows
        // run out before the vertices do (cutout slabs, small sections).
        static constexpr uint32_t kSlotsPerSlab    = 1024;
        static constexpr size_t   kOriginEntryBytes = 16;                       // ivec4
        static constexpr size_t   kSlotBytes        = kSlotsPerSlab * kOriginEntryBytes;
        static constexpr uint32_t kMaxSlabs         = 128;                      // 2 MB of tables
        BufferHandle m_originsUbo = INVALID_BUFFER;

        static bool AllocSlot(Slab& slab, uint16_t& outSlot);
        static void FreeSlot(Slab& slab, uint16_t slot);
    };

} // namespace Render
