// File: src/render/mesh/ChunkMeshData.hpp
#pragma once

#include "MeshBuilder.hpp"
#include <glad/glad.h>
#include <memory>

namespace Render {

    // GPU mesh data for one render layer
    struct MeshData {
        GLuint vbo = 0;        // Vertex buffer object
        GLuint ibo = 0;        // Index buffer object
        GLuint vao = 0;        // Vertex array object
        size_t indexCount = 0; // Number of indices to render
        bool isUploaded = false; // Whether data has been uploaded to GPU

        MeshData() = default;

        // Disable copy constructor and assignment
        MeshData(const MeshData&) = delete;
        MeshData& operator=(const MeshData&) = delete;

        // Enable move constructor and assignment
        MeshData(MeshData&& other) noexcept
            : vbo(other.vbo), ibo(other.ibo), vao(other.vao),
              indexCount(other.indexCount), isUploaded(other.isUploaded) {
            other.vbo = 0;
            other.ibo = 0;
            other.vao = 0;
            other.indexCount = 0;
            other.isUploaded = false;
        }

        MeshData& operator=(MeshData&& other) noexcept {
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

        ~MeshData() {
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

    // Complete GPU mesh for a chunk with all three render layers
    struct ChunkMesh {
        MeshData opaque;      // Solid blocks (depth write, no blend)
        MeshData cutout;      // Alpha-test blocks (leaves, grass)
        MeshData translucent; // Blended blocks (glass, water, ice)

        // Upload all layer data from ChunkMeshData
        void UploadAll(const ChunkMeshData& data);

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

} // namespace Render