// File: src/client/renderer/mesh/SectionMesh.hpp
#pragma once
#include <algorithm>

#include "../core/Vertex.hpp"
#include "../backend/RenderTypes.hpp"
#include "../culling/VisibilitySet.hpp"
#include "TranslucentSort.hpp"
#include "common/world/math/WorldMath.hpp"
#include <vector>
#include <cstdint>

namespace Render {

    // Forward declaration for DestroyAllResources
    class RenderBackend;

    // Plain-old data struct to hold CPU-side mesh buffers for one 16x16x16 section.
    // Indices are 16-bit and relative to the section's own vertex range; a
    // section layer is capped at 65,536 vertices — see the guards in
    // Mesher::GenerateQuad / FluidMeshBuilder. The mega-buffer rebases them to
    // absolute uint32 at upload (ChunkMegaBuffer::INDEX_SIZE), so nothing on
    // the GPU side ever sees these as-is.
    //
    // Vertices are the 16-byte packed TERRAIN format (TerrainVertex: section-
    // relative fixed-point position, slot, uv/sprite, colour). Everything
    // downstream of the mesher — MeshBuildResult float blobs,
    // ChunkMegaBuffer::VERTEX_STRIDE, the terrain shaders — assumes this stride.
    // --- Face-direction groups -------------------------------------------
    // A quad facing +X can only be seen from x greater than its plane, so
    // for a section wholly on the camera's -x side every +X quad is a
    // back face the GPU would discard AFTER running its vertices through the
    // tiler. The opaque and cutout index buffers are therefore laid out in
    // seven contiguous groups by facing, and the renderer draws only the
    // groups that can face the camera (ChunkRenderer::RenderLayerPass) —
    // Sodium's chunk face culling. Pixel-identical to drawing everything, by
    // construction: back-face culling removes exactly these triangles.
    //
    // Group ORDER in the index buffer is chosen so the common cases are one
    // or two contiguous runs: a camera in the (+,+,+) octant of a section
    // draws [Any, +Z, +Y, +X], in (-,-,-) draws [-X, -Y, -Z, Any].
    // kFacingAny holds every quad that is not axis-aligned (cross plants,
    // rotated elements, sloped fluid) — always drawn. The two-sided plant
    // quads (TerrainVertex::kTwoSidedFlag) live there too: their four
    // vertices are indexed twice, once per winding, so culling keeps
    // whichever side faces the camera with no state change.
    enum : uint8_t {
        kFacingNegX = 0, kFacingPosX, kFacingNegY, kFacingPosY, kFacingNegZ, kFacingPosZ, kFacingAny,
        kFacingCount = 7
    };
    // Slot i of the index layout holds facing kFacingGroupOrder[i].
    constexpr uint8_t kFacingGroupOrder[kFacingCount] = {
        kFacingNegX, kFacingNegY, kFacingNegZ, kFacingAny, kFacingPosZ, kFacingPosY, kFacingPosX
    };

    // Facing of a quad from three corners, using the mesher's winding (CCW
    // seen from the front): the normal is (p1-p0) x (p2-p0). Axis-aligned
    // within a hair, or Any.
    inline uint8_t QuadFacing(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2) {
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        const glm::vec3 a = glm::abs(n);
        const float len = a.x + a.y + a.z;
        if (len <= 0.0f) return kFacingAny;
        constexpr float kAxisShare = 0.999f;   // |dominant| / (sum of |components|)
        if (a.x >= kAxisShare * len) return n.x > 0.0f ? kFacingPosX : kFacingNegX;
        if (a.y >= kAxisShare * len) return n.y > 0.0f ? kFacingPosY : kFacingNegY;
        if (a.z >= kAxisShare * len) return n.z > 0.0f ? kFacingPosZ : kFacingNegZ;
        return kFacingAny;
    }

    // Reorder a layer's indices (6 per quad, quad k = indices 6k..6k+5) into
    // the group layout and fill ranges[0..7] with the group boundaries
    // (ranges[i]..ranges[i+1] = slot i, ranges[7] = index count). A facing
    // list that does not match the quad count (an emitter that forgot to
    // record one) leaves the order untouched and files everything under
    // Any, which draws exactly as before.
    inline void GroupIndicesByFacing(std::vector<uint16_t>& idxs, const std::vector<uint8_t>& facing,
                                     uint32_t ranges[kFacingCount + 1]) {
        const size_t quads = idxs.size() / 6;
        for (int i = 0; i <= kFacingCount; ++i) ranges[i] = 0;
        if (facing.size() != quads || idxs.size() != quads * 6) {
            // Everything in the Any slot (slot 3): ranges 0..3 = 0, 4..7 = count.
            for (int i = 0; i < kFacingCount; ++i) {
                ranges[i + 1] = (kFacingGroupOrder[i] == kFacingAny || ranges[i] > 0) ?
                                static_cast<uint32_t>(idxs.size()) : 0;
            }
            return;
        }
        uint32_t slotOf[kFacingCount];
        for (int slot = 0; slot < kFacingCount; ++slot) slotOf[kFacingGroupOrder[slot]] = static_cast<uint32_t>(slot);
        uint32_t counts[kFacingCount] = {};
        for (size_t q = 0; q < quads; ++q) counts[slotOf[facing[q] < kFacingCount ? facing[q] : kFacingAny]] += 6;
        for (int slot = 0; slot < kFacingCount; ++slot) ranges[slot + 1] = ranges[slot] + counts[slot];
        std::vector<uint16_t> sorted(idxs.size());
        uint32_t cursor[kFacingCount];
        for (int slot = 0; slot < kFacingCount; ++slot) cursor[slot] = ranges[slot];
        for (size_t q = 0; q < quads; ++q) {
            const uint32_t slot = slotOf[facing[q] < kFacingCount ? facing[q] : kFacingAny];
            std::copy(idxs.begin() + static_cast<std::ptrdiff_t>(q * 6),
                      idxs.begin() + static_cast<std::ptrdiff_t>(q * 6 + 6),
                      sorted.begin() + cursor[slot]);
            cursor[slot] += 6;
        }
        idxs.swap(sorted);
    }

    struct SectionMesh {
        // Opaque geometry (solid blocks like stone, dirt)
        std::vector<TerrainVertex> opaqueVerts;
        std::vector<uint16_t> opaqueIdxs;

        // Cutout geometry (alpha-test blocks like leaves, grass)
        std::vector<TerrainVertex> cutoutVerts;
        std::vector<uint16_t> cutoutIdxs;

        // One facing per quad (see QuadFacing), recorded by every emitter for
        // the opaque and cutout layers; FinalizeFacingGroups turns them into
        // the grouped index layout + ranges. Translucent stays in draw order
        // (it is re-sorted back to front and is ~1% of vertices).
        std::vector<uint8_t> opaqueFacing;
        std::vector<uint8_t> cutoutFacing;
        uint32_t opaqueFacingRanges[kFacingCount + 1] = {};
        uint32_t cutoutFacingRanges[kFacingCount + 1] = {};

        void FinalizeFacingGroups() {
            GroupIndicesByFacing(opaqueIdxs, opaqueFacing, opaqueFacingRanges);
            GroupIndicesByFacing(cutoutIdxs, cutoutFacing, cutoutFacingRanges);
        }

        // Face map: the per-block records of this layer's greedy-merged
        // rectangles (two RGBA8 texels per block face, see
        // TerrainVertex::Mapped). Uploaded after the layer's vertices into
        // the same mega-buffer region and read by the fragment shader
        // through a buffer texture over the slab.
        std::vector<uint32_t> opaqueFaceMap;
        std::vector<uint32_t> cutoutFaceMap;

        // Translucent geometry (blended blocks like glass, water, ice)
        std::vector<TerrainVertex> translucentVerts;
        std::vector<uint16_t> translucentIdxs;

        // Section position
        Game::Math::ChunkPos chunkPos{0, 0};
        int sectionY = 0;

        // Occlusion data (computed by VisGraph during mesh build)
        VisibilitySet visibilitySet;

        SectionMesh() = default;
        SectionMesh(Game::Math::ChunkPos pos, int secY) : chunkPos(pos), sectionY(secY) {}

        // Clear all mesh data
        void Clear() {
            opaqueVerts.clear();
            opaqueIdxs.clear();
            cutoutVerts.clear();
            cutoutIdxs.clear();
            translucentVerts.clear();
            translucentIdxs.clear();
            opaqueFacing.clear();
            cutoutFacing.clear();
            opaqueFaceMap.clear();
            cutoutFaceMap.clear();
            for (int i = 0; i <= kFacingCount; ++i) { opaqueFacingRanges[i] = 0; cutoutFacingRanges[i] = 0; }
        }

        // Check if any layer has geometry
        bool IsEmpty() const {
            return opaqueVerts.empty() && cutoutVerts.empty() && translucentVerts.empty();
        }

        // Get total vertex count across all layers
        size_t GetTotalVertexCount() const {
            return opaqueVerts.size() + cutoutVerts.size() + translucentVerts.size();
        }

        // Get total index count across all layers
        size_t GetTotalIndexCount() const {
            return opaqueIdxs.size() + cutoutIdxs.size() + translucentIdxs.size();
        }

        // Reserve space for mesh data (optimization)
        void Reserve(size_t estimatedQuads) {
            size_t verts = estimatedQuads * 4;
            size_t indices = estimatedQuads * 6;

            // Distribute estimates across layers (rough approximation)
            size_t opaqueEst = verts * 0.6f;  // Most blocks are opaque
            size_t cutoutEst = verts * 0.2f;  // Some cutout blocks
            size_t transEst = verts * 0.2f;   // Fewer translucent blocks

            opaqueVerts.reserve(opaqueEst);
            opaqueIdxs.reserve(opaqueEst * 6 / 4);
            cutoutVerts.reserve(cutoutEst);
            cutoutIdxs.reserve(cutoutEst * 6 / 4);
            translucentVerts.reserve(transEst);
            translucentIdxs.reserve(transEst * 6 / 4);
        }
    };

    // GPU data for one section.
    // With mega-buffer rendering, per-section GPU handles are no longer stored here.
    // The ChunkMegaBuffer owns all GPU resources; this struct holds counts and identity
    // needed by the rendering pipeline for frustum culling, stats, and draw command lookup.
    struct GPUSectionData {
        // Index counts for rendering
        uint32_t opaqueIndexCount = 0;
        uint32_t cutoutIndexCount = 0;
        uint32_t translucentIndexCount = 0;

        // Vertex counts for accurate statistics
        uint32_t opaqueVertexCount = 0;
        uint32_t cutoutVertexCount = 0;
        uint32_t translucentVertexCount = 0;

        // Section identification
        Game::Math::ChunkPos chunkPos{0, 0};
        int sectionY = 0;

        // Cached mega-buffer draw commands (populated at upload, read during rendering).
        // Eliminates per-frame hash lookups in RenderLayerPass. Offsets are in
        // INDICES, not bytes — the renderer merges adjacent sections by index
        // range and converts to bytes only when it emits the draw. No
        // baseVertex: slab indices are absolute (ChunkMegaBuffer::INDEX_SIZE).
        struct CachedDrawCmd {
            int32_t indexCount = 0;
            uint32_t indexOffset = 0;
            bool valid = false;
            uint32_t slabIndex = 0;
            // Face-direction groups (see kFacingGroupOrder): ABSOLUTE slab
            // index offsets, facingRanges[i]..[i+1] = slot i. hasFacing is
            // false for translucent and for meshes built without groups; the
            // renderer then draws the whole range.
            uint32_t facingRanges[kFacingCount + 1] = {};
            bool hasFacing = false;
            // Per-section index buffer, set only for layers whose pool uses
            // them (translucent). INVALID_BUFFER means indices live in the
            // shared slab IBO and the layer is drawn with one multi-draw.
            BufferHandle ibo = INVALID_BUFFER;
        };
        CachedDrawCmd opaqueDrawCmd;
        CachedDrawCmd cutoutDrawCmd;
        CachedDrawCmd translucentDrawCmd;

        // Back-to-front ordering state for this section's translucent layer.
        // Centroids are kept (12 bytes a quad, versus 96 for the vertices) so a
        // re-sort only has to rewrite the index buffer — the vertex data never
        // moves. Empty for the overwhelming majority of sections, which carry
        // no translucent geometry at all.
        // See mesh/TranslucentSort.hpp for why this is required and not merely
        // a refinement.
        std::vector<glm::vec3> translucentCentroids;
        TranslucentSort::PointOfView translucencyPov;   // invalid until first sort

        // Occlusion culling: which face pairs can see through this section
        VisibilitySet visibilitySet;

        // Upload timestamp for LRU management
        uint64_t lastUploadFrame = 0;
        bool needsUpload = false;

        GPUSectionData() = default;
        GPUSectionData(Game::Math::ChunkPos pos, int secY) : chunkPos(pos), sectionY(secY) {}

        // Check if any layer has renderable geometry
        bool HasGeometry() const {
            return opaqueIndexCount > 0 || cutoutIndexCount > 0 || translucentIndexCount > 0;
        }

        // Get total memory usage estimate
        size_t GetMemoryUsage() const {
            // Vertices are ~2/3 of the index count (4 per 6); uint32 indices
            // on the GPU side, see ChunkMegaBuffer::INDEX_SIZE.
            return (opaqueVertexCount + cutoutVertexCount + translucentVertexCount) * sizeof(TerrainVertex) +
                   (opaqueIndexCount + cutoutIndexCount + translucentIndexCount) * sizeof(uint32_t);
        }

        // No-op: GPU resources are now owned by ChunkMegaBuffer.
        // Kept for API compatibility with legacy code paths (GPUDataPool, ChunkMeshData).
        void DestroyAllResources(RenderBackend* backend);
    };

    // Complete mesh data for one chunk (24 sections)
    struct ChunkMesh {
        static constexpr int SECTIONS_PER_CHUNK = Game::Math::SECTIONS_PER_CHUNK;

        std::vector<SectionMesh> sections;
        Game::Math::ChunkPos chunkPos{0, 0};

        ChunkMesh() {
            sections.resize(SECTIONS_PER_CHUNK);
        }

        explicit ChunkMesh(Game::Math::ChunkPos pos) : chunkPos(pos) {
            sections.resize(SECTIONS_PER_CHUNK);
            for (int i = 0; i < SECTIONS_PER_CHUNK; ++i) {
                sections[i] = SectionMesh(pos, i);
            }
        }

        // Clear all section data
        void Clear() {
            for (auto& section : sections) {
                section.Clear();
            }
        }

        // Get section by Y index (0-23)
        SectionMesh& GetSection(int sectionIndex) {
            return sections[sectionIndex];
        }

        const SectionMesh& GetSection(int sectionIndex) const {
            return sections[sectionIndex];
        }

        // Check if chunk has any geometry
        bool IsEmpty() const {
            for (const auto& section : sections) {
                if (!section.IsEmpty()) {
                    return false;
                }
            }
            return true;
        }

        // Get total geometry stats
        size_t GetTotalVertexCount() const {
            size_t total = 0;
            for (const auto& section : sections) {
                total += section.GetTotalVertexCount();
            }
            return total;
        }

        size_t GetTotalIndexCount() const {
            size_t total = 0;
            for (const auto& section : sections) {
                total += section.GetTotalIndexCount();
            }
            return total;
        }
    };

} // namespace Render