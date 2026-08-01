// File: src/client/renderer/environment/CloudRenderer.hpp
//
// Minecraft clouds, ported from CloudRenderer.java (post-1.21.9 constants:
// 12-block cells, 4 blocks thick, bottom at y=192.33, 0.03 blocks/tick +X
// drift). MC's newest renderer is GPU-driven (texel buffer + gl_VertexID);
// the backend has no texel buffers, so the same cell/face logic builds a CPU
// vertex mesh instead. Mesh is camera-relative and only rebuilt when the
// camera crosses a 12-block cell boundary / crosses the cloud layer / the
// mode or radius changes; per-frame drift is a fractional model translation.
//
// Cloud mode comes from the existing Video Settings "Clouds" option
// (GetRenderClouds(): "true" = Fancy 3D, "fast" = flat, "false" = off).
#pragma once

#include "../backend/RenderTypes.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Render {

    class CloudRenderer {
    public:
        CloudRenderer() = default;
        ~CloudRenderer();

        bool Initialize();
        void Shutdown();

        void Render(const glm::mat4& proj, const glm::mat4& view,
                    const glm::vec3& cameraPos, int renderDistChunks,
                    float partialTick);

    private:
        enum class RelativePos { BelowClouds, InsideClouds, AboveClouds };
        enum class Mode { Off, Fast, Fancy };

        struct Vertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vertex) == 24, "must match GetBlockVertexLayout stride");

        static Mode CurrentMode();
        bool CellOccupied(int cx, int cz) const;
        void EmitFace(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                      int rx, int rz, int dir, bool inside, bool useTopColor) const;
        void BuildCell(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                       int rx, int rz, int cellX, int cellZ,
                       RelativePos rel, Mode mode) const;
        void RebuildMesh(int cellX, int cellZ, RelativePos rel, Mode mode, int radiusCells);
        void DestroyMeshBuffers(bool deferred);

        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;

        ShaderHandle m_shader = INVALID_SHADER;
        TextureHandle m_whiteTexture = INVALID_TEXTURE;

        // clouds.png occupancy grid (alpha >= 10), wraps in both axes.
        std::vector<uint8_t> m_cells;
        int m_texWidth = 0;
        int m_texHeight = 0;

        BufferHandle m_vb = INVALID_BUFFER;
        BufferHandle m_ib = INVALID_BUFFER;
        MeshHandle m_mesh = INVALID_MESH;
        uint32_t m_indexCount = 0;

        // Rebuild keys.
        int m_lastCellX = INT32_MIN;
        int m_lastCellZ = INT32_MIN;
        RelativePos m_lastRel = RelativePos::BelowClouds;
        Mode m_lastMode = Mode::Off;
        int m_lastRadiusCells = -1;

        bool m_initialized = false;
    };

    extern CloudRenderer g_cloudRenderer;

} // namespace Render
