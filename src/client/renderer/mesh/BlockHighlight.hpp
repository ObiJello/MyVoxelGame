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

        // Render the highlight for the given block position
        void Render(const glm::ivec3& blockPos, const glm::mat4& viewProjectionMatrix);

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
