// File: src/client/renderer/mesh/BlockHighlight.hpp
#pragma once

#include <glm/glm.hpp>
#include <optional>

#include "../../input/PlayerController.hpp"
#include "common/physics/RayCast.hpp"
#include "../backend/RenderTypes.hpp"

namespace Render {

    class BlockHighlight {
    public:
        BlockHighlight();
        ~BlockHighlight();

        // Initialize the highlight renderer (call once)
        bool Initialize();

        // Release GPU resources (safe to call multiple times)
        void Shutdown();

        // Render the highlight for the given block position. `shapeMin`/`shapeMax`
        // are the model-shape bounds in [0,1] block-local space — pass the values
        // returned by BlockRegistry::GetBlockShape() so partial-cube blocks
        // (leaf litter, slabs, fences, …) outline their actual geometry instead
        // of the full enclosing cube.
        void Render(const glm::ivec3& blockPos, const glm::mat4& projectionMatrix, const glm::mat4& viewMatrix,
                    const glm::vec3& shapeMin = glm::vec3(0.0f),
                    const glm::vec3& shapeMax = glm::vec3(1.0f));

        // Check if a hit is valid for highlighting (in range, solid block, etc.)
        static bool IsValidHighlight(const std::optional<Game::RaycastHit>& hit);

    private:
        // Backend handles
        ShaderHandle m_shader = INVALID_SHADER;
        MeshHandle m_mesh = INVALID_MESH;
        BufferHandle m_vb = INVALID_BUFFER;
        BufferHandle m_ib = INVALID_BUFFER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // Shader source code (used by GL backend's CreateShader)
        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;
    };

    // Global highlight renderer instance
    extern BlockHighlight g_blockHighlight;

} // namespace Render
