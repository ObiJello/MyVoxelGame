// File: src/client/renderer/mesh/BlockBreakOverlay.hpp
//
// MC-faithful crack overlay drawn over the block currently being mined.
// Mirrors `LevelRenderer.renderBlockDestroyAnimation()` + `RenderType.crumbling`.
//
// Each frame, the client's PlayerController tells us:
//   • which block (world position) is being mined, and
//   • which of the 10 stages (0..9) to draw.
//
// We render a unit-cube re-mesh of that block with the destroy_stage_N
// texture overlaid on every face, with a tiny depth bias to survive Z-fight
// against the underlying chunk mesh.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "../texture/AtlasBuilder.hpp"
#include <glm/glm.hpp>

namespace Render {

    class BlockBreakOverlay {
    public:
        BlockBreakOverlay() = default;
        ~BlockBreakOverlay();

        bool Initialize();
        void Shutdown();

        // stage == -1 hides the overlay. stage in [0..9] selects the crack
        // texture (destroy_stage_<stage>.png) drawn over the given block.
        // `shapeMin`/`shapeMax` are the block's actual model bounds in [0,1]
        // block-local space (BlockRegistry::GetBlockShape). Pass (0,0,0)/(1,1,1)
        // for full cubes; partial blocks (leaf litter, slabs, …) get the crack
        // wrapped around their real geometry instead of a floating full-cube
        // overlay.
        void SetTarget(const glm::ivec3& pos, int stage,
                       const glm::vec3& shapeMin = glm::vec3(0.0f),
                       const glm::vec3& shapeMax = glm::vec3(1.0f));
        void Clear() { m_stage = -1; }

        void Render(const glm::mat4& projectionMatrix, const glm::mat4& viewMatrix);

    private:
        // Backend handles
        ShaderHandle  m_shader = INVALID_SHADER;
        MeshHandle    m_mesh   = INVALID_MESH;
        BufferHandle  m_vb     = INVALID_BUFFER;
        BufferHandle  m_ib     = INVALID_BUFFER;

        // Atlas info (resolved lazily from g_atlasBuilder on first Render call —
        // the atlas isn't built yet when Initialize runs).
        bool          m_uvsResolved = false;
        AtlasUVRect   m_stageUVs[10] = {};
        TextureHandle m_atlasTexture = INVALID_TEXTURE;

        // Current target
        glm::ivec3 m_pos {0, -1024, 0};
        int        m_stage = -1;
        glm::vec3  m_shapeMin {0.0f};
        glm::vec3  m_shapeMax {1.0f};

        // Embedded shader source (used by GL CreateShader fallback when the
        // SPIR-V files for Vulkan aren't found).
        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;

        bool ResolveAtlas();
    };

    extern BlockBreakOverlay g_blockBreakOverlay;

} // namespace Render
