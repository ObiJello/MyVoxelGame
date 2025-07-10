// File: src/render/mesh/ChunkMeshData.hpp - Enhanced for Section-Based Meshing
#pragma once

#include "MeshBuilder.hpp"
#include "../../game/WorldMath.hpp"
#include <glad/glad.h>
#include <memory>
#include <array>
#include <atomic>

namespace Render {

    // GPU mesh data for one render layer of a section
    struct SectionMeshData {
        GLuint vbo = 0;        // Vertex buffer object
        GLuint ibo = 0;        // Index buffer object
        GLuint vao = 0;        // Vertex array object
        size_t indexCount = 0; // Number of indices to render
        bool isUploaded = false; // Whether data has been uploaded to GPU

        SectionMeshData() = default;

        // Disable copy constructor and assignment
        SectionMeshData(const SectionMeshData&) = delete;
        SectionMeshData& operator=(const SectionMeshData&) = delete;

        // Enable move constructor and assignment
        SectionMeshData(SectionMeshData&& other) noexcept
            : vbo(other.vbo), ibo(other.ibo), vao(other.vao),
              indexCount(other.indexCount), isUploaded(other.isUploaded) {
            other.vbo = 0;
            other.ibo = 0;
            other.vao = 0;
            other.indexCount = 0;
            other.isUploaded = false;
        }

        SectionMeshData& operator=(SectionMeshData&& other) noexcept {
            if (this != &other) {
                Cleanup();

                vbo = other.vbo;
                ibo = other.ibo;
                vao = other.vao;
                indexCount = other.indexCount;
                isUploaded = other.isUploaded;

                other.vbo = 0;
                other.ibo = 0;
                other.vao = 0;
                other.indexCount = 0;
                other.isUploaded = false;
            }
            return *this;
        }

        ~SectionMeshData() {
            Cleanup();
        }

        // Upload vertex and index data to GPU
        void Upload(const LayerBuffers& buffers);

        // Render the mesh (assumes shader is already bound)
        void Draw() const;

        // Check if mesh has data to render
        bool IsEmpty() const { return indexCount == 0 || !isUploaded; }

        // Get memory usage in bytes
        size_t GetMemoryUsage() const;

    private:
        void Cleanup();
        void SetupVertexAttributes();
    };

    // Complete GPU mesh for a section with all three render layers
    struct SectionMesh {
        SectionMeshData opaque;      // Solid blocks (depth write, no blend)
        SectionMeshData cutout;      // Alpha-test blocks (leaves, grass)
        SectionMeshData translucent; // Blended blocks (glass, water, ice)

        // Section coordinates within chunk (Y level)
        int sectionY = 0;

        // State tracking
        std::atomic<bool> needsRebuild{false};
        std::atomic<bool> hasData{false};

        // Upload all layer data from SectionMeshData
        void UploadAll(const SectionMeshData& data, int sectionYLevel);

        // Check if any layer has data
        bool IsEmpty() const {
            return opaque.IsEmpty() && cutout.IsEmpty() && translucent.IsEmpty();
        }

        // Get total memory usage across all layers
        size_t GetTotalMemoryUsage() const {
            return opaque.GetMemoryUsage() + cutout.GetMemoryUsage() + translucent.GetMemoryUsage();
        }

        // Get statistics
        struct Stats {
            size_t opaqueIndices = 0;
            size_t cutoutIndices = 0;
            size_t translucentIndices = 0;
            size_t totalMemoryBytes = 0;
        };

        Stats GetStats() const {
            Stats stats;
            stats.opaqueIndices = opaque.indexCount;
            stats.cutoutIndices = cutout.indexCount;
            stats.translucentIndices = translucent.indexCount;
            stats.totalMemoryBytes = GetTotalMemoryUsage();
            return stats;
        }
    };

    // Section mesh data before GPU upload (CPU-side buffers)
    struct SectionMeshData {
        LayerBuffers opaque;      // Solid blocks layer
        LayerBuffers cutout;      // Alpha-test blocks layer
        LayerBuffers translucent; // Blended blocks layer

        int sectionY = 0;         // Section Y coordinate

        void Clear() {
            opaque.Clear();
            cutout.Clear();
            translucent.Clear();
        }

        size_t GetTotalVertices() const {
            return opaque.GetVertexCount() + cutout.GetVertexCount() + translucent.GetVertexCount();
        }

        size_t GetTotalIndices() const {
            return opaque.GetIndexCount() + cutout.GetIndexCount() + translucent.GetIndexCount();
        }

        bool IsEmpty() const {
            return opaque.IsEmpty() && cutout.IsEmpty() && translucent.IsEmpty();
        }
    };

    // Complete chunk mesh containing all sections
    struct ChunkMesh {
        // Array of section meshes - matches Game::Math::SECTIONS_PER_CHUNK (24 sections)
        std::array<std::unique_ptr<SectionMesh>, Game::Math::SECTIONS_PER_CHUNK> sections;

        // Chunk coordinates
        int chunkX = 0;
        int chunkZ = 0;

        // Chunk-level state
        std::atomic<bool> fullyBuilt{false};
        std::atomic<int> sectionsBuilt{0};

        ChunkMesh() {
            // Initialize all sections as nullptr - they'll be created on demand
            for (auto& section : sections) {
                section = nullptr;
            }
        }

        // Upload a specific section
        void UploadSection(int sectionIndex, const SectionMeshData& sectionData);

        // Mark a specific section as needing rebuild
        void MarkSectionForRebuild(int sectionIndex);

        // Check if a section exists and has data
        bool HasSection(int sectionIndex) const {
            return sectionIndex >= 0 &&
                   sectionIndex < Game::Math::SECTIONS_PER_CHUNK &&
                   sections[sectionIndex] &&
                   !sections[sectionIndex]->IsEmpty();
        }

        // Get a specific section (returns nullptr if doesn't exist)
        SectionMesh* GetSection(int sectionIndex) {
            if (sectionIndex >= 0 && sectionIndex < Game::Math::SECTIONS_PER_CHUNK) {
                return sections[sectionIndex].get();
            }
            return nullptr;
        }

        const SectionMesh* GetSection(int sectionIndex) const {
            if (sectionIndex >= 0 && sectionIndex < Game::Math::SECTIONS_PER_CHUNK) {
                return sections[sectionIndex].get();
            }
            return nullptr;
        }

        // Get all non-empty sections
        std::vector<SectionMesh*> GetActiveSections() {
            std::vector<SectionMesh*> activeSections;
            for (auto& section : sections) {
                if (section && !section->IsEmpty()) {
                    activeSections.push_back(section.get());
                }
            }
            return activeSections;
        }

        // Check if entire chunk is empty
        bool IsEmpty() const {
            for (const auto& section : sections) {
                if (section && !section->IsEmpty()) {
                    return false;
                }
            }
            return true;
        }

        // Get total memory usage across all sections
        size_t GetTotalMemoryUsage() const {
            size_t total = 0;
            for (const auto& section : sections) {
                if (section) {
                    total += section->GetTotalMemoryUsage();
                }
            }
            return total;
        }

        // Get chunk statistics
        struct ChunkStats {
            int activeSections = 0;
            size_t totalVertices = 0;
            size_t totalIndices = 0;
            size_t totalMemoryBytes = 0;
            size_t opaqueIndices = 0;
            size_t cutoutIndices = 0;
            size_t translucentIndices = 0;
        };

        ChunkStats GetStats() const {
            ChunkStats stats;
            for (const auto& section : sections) {
                if (section && !section->IsEmpty()) {
                    stats.activeSections++;
                    auto sectionStats = section->GetStats();
                    stats.opaqueIndices += sectionStats.opaqueIndices;
                    stats.cutoutIndices += sectionStats.cutoutIndices;
                    stats.translucentIndices += sectionStats.translucentIndices;
                    stats.totalMemoryBytes += sectionStats.totalMemoryBytes;
                }
            }
            stats.totalIndices = stats.opaqueIndices + stats.cutoutIndices + stats.translucentIndices;
            // Rough vertex estimate (assuming ~1.5 vertices per index on average)
            stats.totalVertices = stats.totalIndices * 3 / 4;
            return stats;
        }

        // Clean up empty sections to save memory
        void CompactSections() {
            for (auto& section : sections) {
                if (section && section->IsEmpty()) {
                    section.reset(); // Delete empty sections
                }
            }
        }
    };

} // namespace Render