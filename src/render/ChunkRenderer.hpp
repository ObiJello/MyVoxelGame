#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include "Frustum.hpp"
#include "../game/Mesher.hpp"     // for Game::MeshData
#include "../game/WorldMath.hpp"  // for CHUNK_SIZE_X, SECTION_HEIGHT, CHUNK_SIZE_Z

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

            // 5) Compute worldOffset using data->chunkXZ and data->sectionIndex
            cm.worldOffset = glm::vec3(
                float(data->chunkXZ.x * Game::Math::CHUNK_SIZE_X),
                float(data->sectionIndex * Game::Math::SECTION_HEIGHT),
                float(data->chunkXZ.y * Game::Math::CHUNK_SIZE_Z)
            );

            // Store metadata for later lookups
            cm.chunkXZ = { data->chunkXZ.x, data->chunkXZ.y };
            cm.sectionIndex = data->sectionIndex;

            return cm;
        }

        // Render this mesh (assumes a shader with uMVP already bound)
        void Draw() const {
            glBindVertexArray(vao);
            glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
            glBindVertexArray(0);
        }

        // Compute this section's AABB in world space for frustum culling
        AABB GetAABB() const {
            glm::vec3 min = worldOffset;
            glm::vec3 max = worldOffset + glm::vec3(
                float(Game::Math::CHUNK_SIZE_X),
                float(Game::Math::SECTION_HEIGHT),
                float(Game::Math::CHUNK_SIZE_Z)
            );
            return AABB{ min, max };
        }
    };

    // Global container of all uploaded meshes (one per section)
    extern std::vector<ChunkMesh> g_chunkMeshes;

    // Called once per finished MeshData to upload a new mesh to the GPU.
    // Reads chunkXZ and sectionIndex from data itself.
    inline void UploadMesh(Game::MeshData* data) {
        ChunkMesh cm = ChunkMesh::FromMeshData(data);
        g_chunkMeshes.push_back(cm);
        delete data;
    }

} // namespace Render