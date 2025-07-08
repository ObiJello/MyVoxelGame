// File: src/render/mesh/ChunkRenderer.hpp
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

namespace Render {

    // Forward declarations
    class Camera;
    class Shader;
    struct Frustum;
    namespace Debug { struct PerformanceMetrics; }

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

        // Construct from vertex/index data
        void Upload(const std::vector<Render::Vertex>& vertices,
                   const std::vector<uint32_t>& indices) {
            if (vertices.empty() || indices.empty()) {
                // Empty mesh - don't create GPU resources
                indexCount = 0;
                return;
            }

            // Generate and bind VAO
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);

            // Upload vertex buffer
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER,
                        vertices.size() * sizeof(Render::Vertex),
                        vertices.data(), GL_STATIC_DRAW);

            // Upload index buffer
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                        indices.size() * sizeof(uint32_t),
                        indices.data(), GL_STATIC_DRAW);

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

            glBindVertexArray(0);
            indexCount = static_cast<GLsizei>(indices.size());
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

        // Draw this layer (assumes shader is already bound)
        void Draw() const {
            if (indexCount > 0 && vao != 0) {
                glBindVertexArray(vao);
                glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
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

        // Construct from layered mesh data
        static ChunkMesh FromLayeredMeshData(const Game::LayeredMeshData* data) {
            ChunkMesh cm;

            // Upload each layer
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

    // ENHANCED: Upload layered mesh data
    inline void UploadLayeredMesh(Game::LayeredMeshData* data) {
        if (!data) {
            Log::Warning("UploadLayeredMesh called with null data");
            return;
        }

        std::unique_ptr<Game::LayeredMeshData> ownedData(data);

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
            meshes.push_back(cm);

            Log::Debug("Added new layered mesh for chunk (%d,%d) section %d - "
                      "Opaque: %zu verts, Cutout: %zu verts, Translucent: %zu verts",
                      ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex,
                      ownedData->opaqueVertices.size(), ownedData->cutoutVertices.size(),
                      ownedData->translucentVertices.size());
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