// File: src/client/renderer/mesh/SectionMesh.hpp
#pragma once

#include "../core/Vertex.hpp"
#include "../backend/RenderTypes.hpp"
#include "../culling/VisibilitySet.hpp"
#include "common/world/math/WorldMath.hpp"
#include <vector>
#include <cstdint>

namespace Render {

    // Forward declaration for DestroyAllResources
    class RenderBackend;

    // Plain-old data struct to hold CPU-side mesh buffers for one 16x16x16 section
    struct SectionMesh {
        // Opaque geometry (solid blocks like stone, dirt)
        std::vector<Vertex> opaqueVerts;
        std::vector<uint32_t> opaqueIdxs;

        // Cutout geometry (alpha-test blocks like leaves, grass)
        std::vector<Vertex> cutoutVerts;
        std::vector<uint32_t> cutoutIdxs;

        // Translucent geometry (blended blocks like glass, water, ice)
        std::vector<Vertex> translucentVerts;
        std::vector<uint32_t> translucentIdxs;

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
        // Eliminates per-frame hash lookups in RenderLayerPass.
        struct CachedDrawCmd {
            int32_t indexCount = 0;
            size_t indexByteOffset = 0;
            int32_t baseVertex = 0;
            bool valid = false;
            uint32_t slabIndex = 0;
        };
        CachedDrawCmd opaqueDrawCmd;
        CachedDrawCmd cutoutDrawCmd;
        CachedDrawCmd translucentDrawCmd;

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
            return (opaqueIndexCount + cutoutIndexCount + translucentIndexCount) *
                   (sizeof(Vertex) + sizeof(uint32_t));
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