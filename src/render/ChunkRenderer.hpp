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

            /* **DEBUG**: Log what coordinates we're receiving
            Log::Debug("FromMeshData: Creating mesh for chunk coords (%d,%d) section %d",
                      data->chunkXZ.x, data->chunkXZ.y, data->sectionIndex);*/

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

            // 4) Set up vertex attributes: pos (0), normal (1), texCoord (2), color (3)
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

            // aColor (location = 3) - NEW for biome tinting
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(
                3,
                4,
                GL_FLOAT,
                GL_FALSE,
                (GLsizei)stride,
                (void*)offsetof(Render::Vertex, color)
            );

            glBindVertexArray(0);
            cm.indexCount = static_cast<GLsizei>(data->indices.size());

            // 5) FIXED: Compute worldOffset accounting for MinY offset
            // The mesh vertices are in section-local coordinates (0-15 for each axis)
            // We need to translate them to world coordinates
            cm.worldOffset = glm::vec3(0.0f);


            // Store metadata for later lookups
            // **CRITICAL FIX**: MeshData.chunkXZ uses .x for X and .y for Z coordinate!
            // We need to store this in ChunkMesh.chunkXZ where .x is X and .z is Z
            cm.chunkXZ.x = data->chunkXZ.x;  // X coordinate
            cm.chunkXZ.z = data->chunkXZ.z;  // Z coordinate (from MeshData.y!)
            cm.sectionIndex = data->sectionIndex;

            /* **DEBUG**: Log what coordinates we stored
            Log::Debug("FromMeshData: Stored as ChunkMesh coords (%d,%d) section %d",
                      cm.chunkXZ.x, cm.chunkXZ.z, cm.sectionIndex);*/

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
            // Calculate the actual world position of this section
            float worldX = static_cast<float>(chunkXZ.x * Game::Math::CHUNK_SIZE_X);
            float worldY = static_cast<float>(Config::MinY + (sectionIndex * Game::Math::SECTION_HEIGHT));
            float worldZ = static_cast<float>(chunkXZ.z * Game::Math::CHUNK_SIZE_Z);

            glm::vec3 min(worldX, worldY, worldZ);
            glm::vec3 max(
                worldX + Game::Math::CHUNK_SIZE_X,
                worldY + Game::Math::SECTION_HEIGHT,
                worldZ + Game::Math::CHUNK_SIZE_Z
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
    // **FIXED**: Upload mesh with proper memory ownership
    inline void UploadMesh(Game::MeshData* data) {
        if (!data) {
            Log::Warning("UploadMesh called with null data");
            return;
        }

        // **CRITICAL**: Take ownership immediately to prevent double-delete
        std::unique_ptr<Game::MeshData> ownedData(data);

        if (ownedData->vertices.empty()) {
            /*Log::Debug("Skipping upload of empty mesh for chunk (%d,%d) section %d",
                      ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex);*/
            // ownedData automatically deleted when it goes out of scope
            return;
        }

        // Find and replace existing mesh for this chunk/section
        auto& meshes = g_chunkMeshes;

        // Look for existing mesh to replace
        for (auto it = meshes.begin(); it != meshes.end(); ++it) {
            if (it->chunkXZ.x == ownedData->chunkXZ.x &&
                it->chunkXZ.z == ownedData->chunkXZ.z &&
                it->sectionIndex == ownedData->sectionIndex) {

                Log::Debug("Replacing existing mesh for chunk (%d,%d) section %d",
                          ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex);

                // **FIXED**: Properly clean up the old mesh
                if (it->vao != 0) {
                    glDeleteVertexArrays(1, &it->vao);
                    it->vao = 0;
                }
                if (it->vbo != 0) {
                    glDeleteBuffers(1, &it->vbo);
                    it->vbo = 0;
                }
                if (it->ebo != 0) {
                    glDeleteBuffers(1, &it->ebo);
                    it->ebo = 0;
                }

                // Replace with new mesh (data is automatically deleted)
                *it = ChunkMesh::FromMeshData(ownedData.get());
                return; // Early return - data is automatically cleaned up
            }
        }

        // No existing mesh found, add new one
        /*Log::Debug("Adding new mesh for chunk (%d,%d) section %d with %zu vertices",
                  ownedData->chunkXZ.x, ownedData->chunkXZ.z, ownedData->sectionIndex,
                  ownedData->vertices.size());*/

        ChunkMesh cm = ChunkMesh::FromMeshData(ownedData.get());
        meshes.push_back(cm);
        // ownedData automatically deleted when it goes out of scope
    }

    // **IMPROVED**: Safer cleanup function for chunk meshes
    inline void RemoveChunkMeshes(Game::Math::ChunkPos pos) {
        auto& meshes = g_chunkMeshes;
        size_t originalSize = meshes.size();

        Log::Debug("RemoveChunkMeshes: Looking for meshes with chunk pos (%d,%d)", pos.x, pos.z);

        // **FIXED**: Use safer cleanup approach
        for (auto it = meshes.begin(); it != meshes.end();) {
            bool shouldRemove = (it->chunkXZ.x == pos.x && it->chunkXZ.z == pos.z);

            if (shouldRemove) {
                Log::Debug("Removing mesh for chunk (%d,%d) section %d",
                          it->chunkXZ.x, it->chunkXZ.z, it->sectionIndex);

                // Clean up OpenGL resources
                if (it->vao != 0) {
                    glDeleteVertexArrays(1, &it->vao);
                }
                if (it->vbo != 0) {
                    glDeleteBuffers(1, &it->vbo);
                }
                if (it->ebo != 0) {
                    glDeleteBuffers(1, &it->ebo);
                }

                // Remove from vector
                it = meshes.erase(it);
            } else {
                ++it;
            }
        }

        size_t removedCount = originalSize - meshes.size();
        Log::Info("RemoveChunkMeshes: removed %zu meshes for chunk (%d,%d)",
                 removedCount, pos.x, pos.z);
    }

} // namespace Render