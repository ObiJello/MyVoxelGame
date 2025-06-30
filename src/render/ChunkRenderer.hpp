#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include "Frustum.hpp"
#include "Log.hpp"
#include "../game/Mesher.hpp"     // for Game::MeshData
#include "../game/WorldMath.hpp"  // for CHUNK_SIZE_X, SECTION_HEIGHT, CHUNK_SIZE_Z
#include "../core/Config.hpp"     // for MinY offset

namespace Render {

    // A single GPU‐resident mesh for one 16×16×16 section.
    struct ChunkMesh {
        GLuint vao         = 0;
        GLuint vbo         = 0;
        GLuint ebo         = 0;
        GLsizei indexCount = 0;
        glm::vec3 worldOffset{ 0.0f };
        Game::Math::ChunkPos chunkXZ{};  // coordinates of parent chunk
        int sectionIndex = 0;            // which section within the chunk

        // **NEW**: Add timestamp for mesh replacement logic
        std::chrono::steady_clock::time_point uploadTime;

        // Construct from MeshData. Reads chunkXZ and sectionIndex from data.
        static ChunkMesh FromMeshData(const Game::MeshData* data) {
            ChunkMesh cm;

            // 1) Generate and bind VAO
            glGenVertexArrays(1, &cm.vao);
            glBindVertexArray(cm.vao);

            // 2) Upload vertex buffer
            glGenBuffers(1, &cm.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, cm.vbo);
            glBufferData(
                GL_ARRAY_BUFFER,
                data->vertices.size() * sizeof(data->vertices[0]),
                data->vertices.data(),
                GL_STATIC_DRAW
            );

            // 3) Upload index buffer
            glGenBuffers(1, &cm.ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cm.ebo);
            glBufferData(
                GL_ELEMENT_ARRAY_BUFFER,
                data->indices.size() * sizeof(uint32_t),
                data->indices.data(),
                GL_STATIC_DRAW
            );

            // 4) Set up vertex attributes: pos (location=0), normal (location=1), texCoord (location=2)
            constexpr size_t stride = sizeof(Render::Vertex);

            // aPos (location = 0)
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(
                0,
                3,
                GL_FLOAT,
                GL_FALSE,
                (GLsizei)stride,
                (void*)offsetof(Render::Vertex, pos)
            );

            // aNormal (location = 1)
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(
                1,
                3,
                GL_FLOAT,
                GL_FALSE,
                (GLsizei)stride,
                (void*)offsetof(Render::Vertex, nrm)
            );

            // aTexCoord (location = 2)
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(
                2,
                2,
                GL_FLOAT,
                GL_FALSE,
                (GLsizei)stride,
                (void*)offsetof(Render::Vertex, uv)
            );

            glBindVertexArray(0);
            cm.indexCount = static_cast<GLsizei>(data->indices.size());

            // 5) FIXED: Compute worldOffset accounting for MinY offset
            // The mesh vertices are in section-local coordinates (0-15 for each axis)
            // We need to translate them to world coordinates
            cm.worldOffset = glm::vec3(
                float(data->chunkXZ.x * Game::Math::CHUNK_SIZE_X),
                float(Config::MinY + (data->sectionIndex * Game::Math::SECTION_HEIGHT)), // FIXED: Add MinY offset
                float(data->chunkXZ.y * Game::Math::CHUNK_SIZE_Z)
            );

            // Store metadata for later lookups
            cm.chunkXZ = { data->chunkXZ.x, data->chunkXZ.y };
            cm.sectionIndex = data->sectionIndex;

            // **NEW**: Record upload time
            cm.uploadTime = std::chrono::steady_clock::now();

            return cm;
        }

        // **NEW**: Cleanup method
        void Cleanup() const {
            if (vao != 0) {
                glDeleteVertexArrays(1, &vao);
            }
            if (vbo != 0) {
                glDeleteBuffers(1, &vbo);
            }
            if (ebo != 0) {
                glDeleteBuffers(1, &ebo);
            }
        }

        // Render this mesh (assumes a shader with uMVP already bound)
        void Draw() const {
            glBindVertexArray(vao);
            glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
            glBindVertexArray(0);
        }

        // FIXED: Compute this section's AABB in world space for frustum culling
        AABB GetAABB() const {
            glm::vec3 min = worldOffset;
            glm::vec3 max = worldOffset + glm::vec3(
                float(Game::Math::CHUNK_SIZE_X),
                float(Game::Math::SECTION_HEIGHT),
                float(Game::Math::CHUNK_SIZE_Z)
            );
            return AABB{ min, max };
        }

        // **NEW**: Check if this mesh matches chunk/section
        bool Matches(Game::Math::ChunkPos pos, int section) const {
            return chunkXZ.x == pos.x && chunkXZ.z == pos.z && sectionIndex == section;
        }
    };

    // Global container of all uploaded meshes (one per section)
    extern std::vector<ChunkMesh> g_chunkMeshes;

    // **IMPROVED**: Upload mesh with proper replacement logic
    inline void UploadMesh(Game::MeshData* data) {
        if (!data) {
            return;
        }

        if (data->vertices.empty()) {
            Log::Debug("Skipping upload of empty mesh for chunk (%d,%d) section %d",
                      data->chunkXZ.x, data->chunkXZ.y, data->sectionIndex);
            delete data;
            return;
        }

        // **CRITICAL FIX**: Find and replace existing mesh for this chunk/section
        auto& meshes = g_chunkMeshes;

        // Look for existing mesh to replace
        for (auto it = meshes.begin(); it != meshes.end(); ++it) {
            if (it->chunkXZ.x == data->chunkXZ.x &&
                it->chunkXZ.z == data->chunkXZ.y &&
                it->sectionIndex == data->sectionIndex) {

                Log::Debug("Replacing existing mesh for chunk (%d,%d) section %d",
                          data->chunkXZ.x, data->chunkXZ.y, data->sectionIndex);

                // Clean up the old mesh
                glDeleteVertexArrays(1, &it->vao);
                glDeleteBuffers(1, &it->vbo);
                glDeleteBuffers(1, &it->ebo);

                // Replace with new mesh
                *it = ChunkMesh::FromMeshData(data);
                delete data;
                return;
                }
        }

        // No existing mesh found, add new one
        Log::Debug("Adding new mesh for chunk (%d,%d) section %d with %zu vertices",
                  data->chunkXZ.x, data->chunkXZ.y, data->sectionIndex, data->vertices.size());

        ChunkMesh cm = ChunkMesh::FromMeshData(data);
        meshes.push_back(cm);
        delete data;
    }

    // **NEW**: Safe cleanup function for chunk meshes
    inline void RemoveChunkMeshes(Game::Math::ChunkPos pos) {
        auto& meshes = g_chunkMeshes;

        meshes.erase(
            std::remove_if(meshes.begin(), meshes.end(),
                [&pos](const ChunkMesh& cm) {
                    bool shouldRemove = (cm.chunkXZ.x == pos.x && cm.chunkXZ.z == pos.z);
                    if (shouldRemove) {
                        cm.Cleanup();
                    }
                    return shouldRemove;
                }),
            meshes.end()
        );
    }

} // namespace Render