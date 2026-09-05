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
#include <string>
#include <vector>

namespace Render {

    class CloudRenderer {
    public:
        CloudRenderer() = default;
        ~CloudRenderer();

        bool Initialize();
        void Shutdown();

        // Use another clouds.png (absolute path; empty = the game's own).
        // A resource-pack sky that ships its own clouds.png — typically a
        // 1×1 transparent pixel that turns the cloud layer off because the
        // clouds are painted into its sky — is applied through this. A
        // no-op when the path is the one in use; a change re-decodes the
        // occupancy grid and forces the next Render to rebuild the mesh.
        void SetCloudTexture(const std::string& absolutePath);
        // Resource pack reload: read the cell grid again from whatever
        // clouds.png now resolves to.
        void ReloadTexture();

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
        bool LoadCellGrid(const std::string& path);
        bool CellOccupied(int cx, int cz) const;
        void EmitFace(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                      int rx, int rz, int dir, bool inside, bool useTopColor) const;
        void BuildCell(std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                       int rx, int rz, int cellX, int cellZ,
                       RelativePos rel, Mode mode) const;
        void RebuildMesh(int cellX, int cellZ, RelativePos rel, Mode mode, int radiusCells);
        void DestroyMeshBuffers(bool deferred);
        void DestroySlot(size_t slot, bool deferred);

        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;

        ShaderHandle m_shader = INVALID_SHADER;
        TextureHandle m_whiteTexture = INVALID_TEXTURE;

        // clouds.png occupancy grid (alpha >= 10), wraps in both axes.
        std::vector<uint8_t> m_cells;
        int m_texWidth = 0;
        int m_texHeight = 0;
        std::string m_cellsPath;        // the clouds.png the grid came from
        std::string m_overridePath;     // SetCloudTexture's path, empty = vanilla

        // GPU mesh, double-buffered. A rebuild writes into host-visible
        // (Dynamic) buffers IN PLACE with UpdateBuffer — on Vulkan that is a
        // plain memcpy with no GPU sync, and the previous frame may still be
        // reading the cloud mesh. So each rebuild targets the slot the
        // previous rebuild did NOT use: with two frames in flight and at
        // most one rebuild per frame, that slot was last read two or more
        // frames ago, and BeginFrame's fence wait has already retired it.
        // Buffers only get recreated (deferred-destroy + new) when a rebuild
        // needs more capacity than the slot has; before this, every rebuild
        // was two Static uploads = two full vkQueueWaitIdle drains.
        struct MeshSlot {
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            MeshHandle mesh = INVALID_MESH;
            size_t vbCapacity = 0;   // bytes
            size_t ibCapacity = 0;   // bytes
        };
        MeshSlot m_slots[2];
        size_t m_activeSlot = 0;     // slot the last rebuild filled
        uint32_t m_indexCount = 0;
        // Scratch reused across rebuilds so a cell crossing does not reallocate.
        std::vector<Vertex> m_scratchVerts;
        std::vector<uint32_t> m_scratchIndices;

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
