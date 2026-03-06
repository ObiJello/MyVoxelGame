// File: src/client/renderer/mesh/SectionMesh.hpp
#pragma once

#include "../core/Vertex.hpp"
#include "../backend/RenderTypes.hpp"
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

    // GPU data for one section (holds backend resource handles)
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

        // Backend resource handles
        MeshHandle opaqueMesh = INVALID_MESH;
        MeshHandle cutoutMesh = INVALID_MESH;
        MeshHandle translucentMesh = INVALID_MESH;
        BufferHandle opaqueVB = INVALID_BUFFER, opaqueIB = INVALID_BUFFER;
        BufferHandle cutoutVB = INVALID_BUFFER, cutoutIB = INVALID_BUFFER;
        BufferHandle translucentVB = INVALID_BUFFER, translucentIB = INVALID_BUFFER;

        // Upload timestamp for LRU management
        uint64_t lastUploadFrame = 0;
        bool needsUpload = false;

        GPUSectionData() = default;
        GPUSectionData(Game::Math::ChunkPos pos, int secY) : chunkPos(pos), sectionY(secY) {}

        // Check if any layer has renderable geometry
        bool HasGeometry() const {
            return opaqueIndexCount > 0 || cutoutIndexCount > 0 || translucentIndexCount > 0;
        }

        // Check if GPU buffers are allocated
        bool IsUploaded() const {
            return opaqueMesh != INVALID_MESH || cutoutMesh != INVALID_MESH || translucentMesh != INVALID_MESH;
        }

        // Get total memory usage estimate
        size_t GetMemoryUsage() const {
            return (opaqueIndexCount + cutoutIndexCount + translucentIndexCount) *
                   (sizeof(Vertex) + sizeof(uint32_t));
        }

        // Destroy all backend resources and reset handles/counts
        // Pass the backend pointer to avoid including RenderBackend.hpp in this header
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