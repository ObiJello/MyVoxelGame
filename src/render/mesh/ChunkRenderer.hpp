// File: src/render/mesh/ChunkRenderer.hpp (CRITICAL FIXES)
#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include "../gfx/Frustum.hpp"
#include "Log.hpp"
#include "Mesher.hpp"
#include "../../game/WorldMath.hpp"
#include "../../core/Config.hpp"
#include <chrono>
#include <memory>
#include "../gfx/Camera.hpp"
#include "../gfx/Shader.hpp"
#include "../debug/DebugSystem.hpp"

namespace Render {

    // Rendering statistics for performance monitoring
    struct LayeredRenderStats {
        int opaqueDrawCalls = 0;
        int cutoutDrawCalls = 0;
        int translucentDrawCalls = 0;
        size_t opaqueVertices = 0;
        size_t cutoutVertices = 0;
        size_t translucentVertices = 0;
        float sortTime = 0.0f;
        int totalVisibleChunks = 0;
    };

    // Enhanced layered rendering functions
    void RenderLayeredScene(const Camera& camera, const Shader& blockShader,
                           const glm::mat4& proj, const glm::mat4& view, const Frustum& frustum,
                           Debug::PerformanceMetrics& metrics);

    LayeredRenderStats RenderLayeredChunks(const Camera& camera, const Shader& blockShader,
                                          const glm::mat4& proj, const glm::mat4& view,
                                          const Frustum& frustum);

    LayeredRenderStats GetLastRenderStats();
    bool ValidateLayeredRendering();
    void RegenerateAllChunksLayered();

    // Global atlas builder reference
    extern std::unique_ptr<AtlasBuilder> g_atlasBuilder;

    // Enumeration for different render layers
    enum class RenderLayer {
        Opaque = 0,     // Solid blocks
        Cutout = 1,     // Alpha-test blocks (leaves, glass)
        Translucent = 2 // Fluids, transparent blocks
    };

    // Single mesh data for one render layer
    struct LayerMeshData {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei indexCount = 0;

        // CRITICAL: Safer upload with validation
        void Upload(const std::vector<Render::Vertex>& vertices,
                   const std::vector<uint32_t>& indices) {

            // Clear any existing resources first
            Cleanup();

            if (vertices.empty() || indices.empty()) {
                indexCount = 0;
                return;
            }

            // CRITICAL: Validate input data sizes
            if (vertices.size() > 1000000) { // 1M vertex limit
                Log::Error("LayerMeshData::Upload: Too many vertices (%zu)", vertices.size());
                return;
            }

            if (indices.size() > 1500000) { // 1.5M index limit
                Log::Error("LayerMeshData::Upload: Too many indices (%zu)", indices.size());
                return;
            }

            // Log large uploads for monitoring
            if (vertices.size() > 10000) {
                Log::Debug("LayerMeshData::Upload: Large mesh - %zu vertices, %zu indices",
                          vertices.size(), indices.size());
            }

            try {
                // Generate and bind VAO
                glGenVertexArrays(1, &vao);
                if (vao == 0) {
                    Log::Error("Failed to generate VAO");
                    return;
                }
                glBindVertexArray(vao);

                // Upload vertex buffer
                glGenBuffers(1, &vbo);
                if (vbo == 0) {
                    Log::Error("Failed to generate VBO");
                    glDeleteVertexArrays(1, &vao);
                    vao = 0;
                    return;
                }

                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER,
                            vertices.size() * sizeof(Render::Vertex),
                            vertices.data(), GL_STATIC_DRAW);

                // Check for OpenGL errors after vertex upload
                GLenum error = glGetError();
                if (error != GL_NO_ERROR) {
                    Log::Error("OpenGL error uploading vertices: 0x%x", error);
                    Cleanup();
                    return;
                }

                // Upload index buffer
                glGenBuffers(1, &ebo);
                if (ebo == 0) {
                    Log::Error("Failed to generate EBO");
                    Cleanup();
                    return;
                }

                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                            indices.size() * sizeof(uint32_t),
                            indices.data(), GL_STATIC_DRAW);

                // Check for OpenGL errors after index upload
                error = glGetError();
                if (error != GL_NO_ERROR) {
                    Log::Error("OpenGL error uploading indices: 0x%x", error);
                    Cleanup();
                    return;
                }

                // Set up vertex attributes: pos (0), normal (1), texCoord (2), color (3)
                constexpr size_t stride = sizeof(Render::Vertex);

                // aPos (location = 0)
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                    (void*)offsetof(Render::Vertex, pos));

                // aNormal (location = 1)
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                    (void*)offsetof(Render::Vertex, nrm));

                // aTexCoord (location = 2)
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                    (void*)offsetof(Render::Vertex, uv));

                // aColor (location = 3)
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                    (void*)offsetof(Render::Vertex, color));

                // Check for OpenGL errors after vertex attribute setup
                error = glGetError();
                if (error != GL_NO_ERROR) {
                    Log::Error("OpenGL error setting vertex attributes: 0x%x", error);
                    Cleanup();
                    return;
                }

                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

                indexCount = static_cast<GLsizei>(indices.size());

                Log::Debug("LayerMeshData uploaded successfully: %zu vertices, %zu indices, VAO=%u",
                          vertices.size(), indices.size(), vao);

            } catch (const std::exception& e) {
                Log::Error("Exception in LayerMeshData::Upload: %s", e.what());
                Cleanup();
            }
        }

        // Cleanup GPU resources
        void Cleanup() {
            if (vao != 0) {
                glDeleteVertexArrays(1, &vao);
                vao = 0;
            }
            if (vbo != 0) {
                glDeleteBuffers(1, &vbo);
                vbo = 0;
            }
            if (ebo != 0) {
                glDeleteBuffers(1, &ebo);
                ebo = 0;
            }
            indexCount = 0;
        }

        // Draw this layer (assumes shader is already bound and valid)
        void Draw() const {
            if (indexCount > 0 && vao != 0) {
                // CRITICAL: Validate OpenGL state before drawing
                GLint currentProgram;
                glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
                if (currentProgram == 0) {
                    Log::Warning("LayerMeshData::Draw: No shader program bound");
                    return;
                }

                glBindVertexArray(vao);

                // Check for OpenGL errors before draw call
                GLenum error = glGetError();
                if (error != GL_NO_ERROR) {
                    Log::Warning("OpenGL error before draw: 0x%x (VAO=%u)", error, vao);
                    glBindVertexArray(0);
                    return;
                }

                glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);

                // Check for OpenGL errors after draw call
                error = glGetError();
                if (error != GL_NO_ERROR) {
                    static int errorCount = 0;
                    if (++errorCount % 100 == 1) { // Log every 100th error to avoid spam
                        Log::Warning("OpenGL error in draw call: 0x%x (logged %d times)", error, errorCount);
                    }
                }

                glBindVertexArray(0);
            }
        }

        // Check if this layer has renderable geometry
        bool HasGeometry() const {
            return indexCount > 0 && vao != 0;
        }
    };

    // Enhanced chunk mesh with three render layers
    struct ChunkMesh {
        // Three render layers
        LayerMeshData opaque;      // Solid blocks
        LayerMeshData cutout;      // Alpha-test blocks (leaves, glass)
        LayerMeshData translucent; // Fluids, stained glass, etc.

        // Metadata
        glm::vec3 worldOffset{0.0f};
        Game::Math::ChunkPos chunkXZ{};
        int sectionIndex = 0;
        std::chrono::steady_clock::time_point uploadTime;

        // CRITICAL: Enhanced construction with validation
        static ChunkMesh FromLayeredMeshData(const Game::LayeredMeshData* data) {
            if (!data) {
                Log::Error("ChunkMesh::FromLayeredMeshData: null data");
                return ChunkMesh{}; // Return empty mesh
            }

            // CRITICAL: Validate input data
            size_t totalVertices = data->GetTotalVertexCount();
            if (totalVertices > 100000) {
                Log::Error("ChunkMesh::FromLayeredMeshData: Suspicious vertex count %zu for chunk (%d,%d) section %d",
                          totalVertices, data->chunkXZ.x, data->chunkXZ.z, data->sectionIndex);
                return ChunkMesh{}; // Return empty mesh
            }

            ChunkMesh cm;

            try {
                // Upload each layer with validation
                cm.opaque.Upload(data->opaqueVertices, data->opaqueIndices);
                cm.cutout.Upload(data->cutoutVertices, data->cutoutIndices);
                cm.translucent.Upload(data->translucentVertices, data->translucentIndices);

                // Store metadata
                cm.chunkXZ = data->chunkXZ;
                cm.sectionIndex = data->sectionIndex;
                cm.uploadTime = std::chrono::steady_clock::now();

                // Calculate world offset for this section
                float worldX = static_cast<float>(data->chunkXZ.x * Game::Math::CHUNK_SIZE_X);
                float worldY = static_cast<float>(Config::MinY + (data->sectionIndex * Game::Math::SECTION_HEIGHT));
                float worldZ = static_cast<float>(data->chunkXZ.z * Game::Math::CHUNK_SIZE_Z);
                cm.worldOffset = glm::vec3(worldX, worldY, worldZ);

                Log::Debug("ChunkMesh created successfully for chunk (%d,%d) section %d",
                          data->chunkXZ.x, data->chunkXZ.z, data->sectionIndex);

            } catch (const std::exception& e) {
                Log::Error("Exception creating ChunkMesh: %s", e.what());
                cm.Cleanup(); // Clean up any partial state
                return ChunkMesh{}; // Return empty mesh
            }

            return cm;
        }

        // Cleanup all layers
        void Cleanup() {
            opaque.Cleanup();
            cutout.Cleanup();
            translucent.Cleanup();
        }

        // Draw specific layer
        void DrawLayer(RenderLayer layer) const {
            switch (layer) {
                case RenderLayer::Opaque:
                    opaque.Draw();
                    break;
                case RenderLayer::Cutout:
                    cutout.Draw();
                    break;
                case RenderLayer::Translucent:
                    translucent.Draw();
                    break;
            }
        }

        // Get AABB for frustum culling
        AABB GetAABB() const {
            float worldX = static_cast<float>(chunkXZ.x * Game::Math::CHUNK_SIZE_X);
            float worldY = static_cast<float>(Config::MinY + (sectionIndex * Game::Math::SECTION_HEIGHT));
            float worldZ = static_cast<float>(chunkXZ.z * Game::Math::CHUNK_SIZE_Z);

            glm::vec3 min(worldX, worldY, worldZ);
            glm::vec3 max(
                worldX + Game::Math::CHUNK_SIZE_X,
                worldY + Game::Math::SECTION_HEIGHT,
                worldZ + Game::Math::CHUNK_SIZE_Z
            );

            return AABB{min, max};
        }

        // Check if this mesh matches chunk/section
        bool Matches(Game::Math::ChunkPos pos, int section) const {
            return chunkXZ.x == pos.x && chunkXZ.z == pos.z && sectionIndex == section;
        }

        // Check if any layer has geometry
        bool HasAnyGeometry() const {
            return opaque.HasGeometry() || cutout.HasGeometry() || translucent.HasGeometry();
        }

        // Get distance from camera (for translucent sorting)
        float GetDistanceFromCamera(const glm::vec3& cameraPos) const {
            float centerX = static_cast<float>(chunkXZ.x * Game::Math::CHUNK_SIZE_X) + Game::Math::CHUNK_SIZE_X * 0.5f;
            float centerY = static_cast<float>(Config::MinY + (sectionIndex * Game::Math::SECTION_HEIGHT)) + Game::Math::SECTION_HEIGHT * 0.5f;
            float centerZ = static_cast<float>(chunkXZ.z * Game::Math::CHUNK_SIZE_Z) + Game::Math::CHUNK_SIZE_Z * 0.5f;

            glm::vec3 center(centerX, centerY, centerZ);
            return glm::length(cameraPos - center);
        }

        // Get total triangle count across all layers
        size_t GetTotalTriangleCount() const {
            return (opaque.indexCount + cutout.indexCount + translucent.indexCount) / 3;
        }
    };

    // Global container of all uploaded meshes
    extern std::vector<ChunkMesh> g_chunkMeshes;

    // ENHANCED: Upload layered mesh data with comprehensive error handling
    inline void UploadLayeredMesh(Game::LayeredMeshData* data) {
        if (!data) {
            Log::Warning("UploadLayeredMesh called with null data");
            return;
        }

        std::unique_ptr<Game::LayeredMeshData> ownedData(data);

        // CRITICAL: Validate the data before processing
        if (ownedData->sectionIndex < 0 || ownedData->sectionIndex >= Game::Math::SECTIONS_PER_CHUNK) {
            Log::Error("UploadLayeredMesh: Invalid section index %d", ownedData->sectionIndex);
            return;
        }

        // CRITICAL: Check for corrupted vertex counts
        size_t totalVertices = ownedData->GetTotalVertexCount();
        if (totalVertices > 100000) {
            Log::Error("UploadLayeredMesh: Corrupted vertex count %zu for chunk (%d,%d) section %d - discarding",
                      totalVertices, ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex);
            return;
        }

        // Find and replace existing mesh for this chunk/section
        auto& meshes = g_chunkMeshes;

        for (auto it = meshes.begin(); it != meshes.end(); ++it) {
            if (it->chunkXZ.x == ownedData->chunkXZ.x &&
                it->chunkXZ.z == ownedData->chunkXZ.z &&
                it->sectionIndex == ownedData->sectionIndex) {

                // Check if all layers are empty
                if (!ownedData->HasAnyGeometry()) {
                    Log::Debug("Removing mesh for fully empty section: chunk (%d,%d) section %d",
                              ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex);

                    // Clean up existing mesh
                    it->Cleanup();
                    meshes.erase(it);
                    return;
                } else {
                    // Replace with new mesh
                    it->Cleanup();
                    *it = ChunkMesh::FromLayeredMeshData(ownedData.get());

                    // Validate the new mesh was created successfully
                    if (!it->HasAnyGeometry()) {
                        Log::Warning("Failed to create replacement mesh for chunk (%d,%d) section %d",
                                   ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex);
                        meshes.erase(it);
                        return;
                    }

                    Log::Debug("Updated layered mesh for chunk (%d,%d) section %d - "
                              "Opaque: %zu verts, Cutout: %zu verts, Translucent: %zu verts",
                              ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex,
                              ownedData->opaqueVertices.size(), ownedData->cutoutVertices.size(),
                              ownedData->translucentVertices.size());
                    return;
                }
            }
        }

        // No existing mesh found - add new one if it has data
        if (ownedData->HasAnyGeometry()) {
            ChunkMesh cm = ChunkMesh::FromLayeredMeshData(ownedData.get());

            // Validate the mesh was created successfully
            if (cm.HasAnyGeometry()) {
                meshes.push_back(cm);

                Log::Debug("Added new layered mesh for chunk (%d,%d) section %d - "
                          "Opaque: %zu verts, Cutout: %zu verts, Translucent: %zu verts",
                          ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex,
                          ownedData->opaqueVertices.size(), ownedData->cutoutVertices.size(),
                          ownedData->translucentVertices.size());
            } else {
                Log::Warning("Failed to create new mesh for chunk (%d,%d) section %d",
                           ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex);
            }
        }
    }

    // Legacy upload function for backward compatibility
    inline void UploadMesh(Game::MeshData* data) {
        if (!data) {
            Log::Warning("UploadMesh called with null data");
            return;
        }

        // Convert legacy MeshData to LayeredMeshData (all goes to opaque layer)
        auto layeredData = std::make_unique<Game::LayeredMeshData>();
        layeredData->opaqueVertices = std::move(data->vertices);
        layeredData->opaqueIndices = std::move(data->indices);
        layeredData->chunkXZ = data->chunkXZ;
        layeredData->sectionIndex = data->sectionIndex;

        // Upload using new system
        UploadLayeredMesh(layeredData.release());

        // Clean up original data
        delete data;
    }

    // Remove chunk meshes
    inline void RemoveChunkMeshes(Game::Math::ChunkPos pos) {
        auto& meshes = g_chunkMeshes;

        for (auto it = meshes.begin(); it != meshes.end();) {
            bool shouldRemove = (it->chunkXZ.x == pos.x && it->chunkXZ.z == pos.z);

            if (shouldRemove) {
                it->Cleanup();
                it = meshes.erase(it);
            } else {
                ++it;
            }
        }
    }

} // namespace Render