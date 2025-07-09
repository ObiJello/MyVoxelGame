// File: src/render/mesh/ChunkMeshData.cpp
#include "ChunkMeshData.hpp"
#include "../../core/Log.hpp"
#include "../Vertex.hpp"

namespace Render {

    void MeshData::Upload(const LayerBuffers& buffers) {
        if (buffers.IsEmpty()) {
            Cleanup();
            return;
        }

        // Clean up existing resources if any
        if (isUploaded) {
            Cleanup();
        }

        // Generate buffers
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);

        if (vao == 0 || vbo == 0 || ibo == 0) {
            Log::Error("Failed to generate OpenGL buffers for mesh data");
            Cleanup();
            return;
        }

        // Bind VAO first
        glBindVertexArray(vao);

        // Upload vertex data
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                    buffers.verts.size() * sizeof(Vertex),
                    buffers.verts.data(),
                    GL_STATIC_DRAW);

        // Upload index data
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    buffers.indices.size() * sizeof(uint32_t),
                    buffers.indices.data(),
                    GL_STATIC_DRAW);

        // Set up vertex attributes
        SetupVertexAttributes();

        // Unbind VAO
        glBindVertexArray(0);

        // Store count and mark as uploaded
        indexCount = buffers.indices.size();
        isUploaded = true;

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            Log::Error("OpenGL error during mesh upload: 0x%x", error);
            Cleanup();
            return;
        }

        Log::Debug("Uploaded mesh: %zu vertices, %zu indices",
                  buffers.verts.size(), buffers.indices.size());
    }

    void MeshData::Draw() const {
        if (IsEmpty()) {
            return;
        }

        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, nullptr);

        // Unbind VAO (optional, but good practice)
        glBindVertexArray(0);
    }

    size_t MeshData::GetMemoryUsage() const {
        if (!isUploaded) {
            return 0;
        }

        // Estimate GPU memory usage
        // Each vertex is sizeof(Vertex), each index is sizeof(uint32_t)
        size_t vertexDataSize = 0;
        size_t indexDataSize = indexCount * sizeof(uint32_t);

        // Get vertex buffer size from OpenGL
        if (vbo != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            GLint bufferSize;
            glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &bufferSize);
            vertexDataSize = static_cast<size_t>(bufferSize);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        return vertexDataSize + indexDataSize;
    }

    void MeshData::Cleanup() {
        if (vao != 0) {
            glDeleteVertexArrays(1, &vao);
            vao = 0;
        }
        if (vbo != 0) {
            glDeleteBuffers(1, &vbo);
            vbo = 0;
        }
        if (ibo != 0) {
            glDeleteBuffers(1, &ibo);
            ibo = 0;
        }

        indexCount = 0;
        isUploaded = false;
    }

    void MeshData::SetupVertexAttributes() {
        // Set up vertex attributes to match Vertex structure
        // See src/render/Vertex.hpp for the layout

        // Position (location 0)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             (void*)offsetof(Vertex, pos));

        // Normal (location 1)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             (void*)offsetof(Vertex, nrm));

        // Texture coordinates (location 2)
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             (void*)offsetof(Vertex, uv));

        // Color (location 3)
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             (void*)offsetof(Vertex, color));

        // Ambient occlusion (location 4) - convert from uint8_t to float
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                             (void*)offsetof(Vertex, ao));
    }

    void ChunkMesh::UploadAll(const ChunkMeshData& data) {
        // Upload each layer
        opaque.Upload(data.opaque);
        cutout.Upload(data.cutout);
        translucent.Upload(data.translucent);

        // Log statistics
        auto stats = GetStats();
        if (stats.opaqueIndices > 0 || stats.cutoutIndices > 0 || stats.translucentIndices > 0) {
            Log::Debug("ChunkMesh uploaded: opaque=%zu, cutout=%zu, translucent=%zu indices (%.2f KB)",
                      stats.opaqueIndices, stats.cutoutIndices, stats.translucentIndices,
                      stats.totalMemoryBytes / 1024.0f);
        }
    }

} // namespace Render