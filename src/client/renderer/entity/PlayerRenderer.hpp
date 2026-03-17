// File: src/client/renderer/entity/PlayerRenderer.hpp
#pragma once

#include "../backend/RenderTypes.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include <glm/glm.hpp>

namespace Render {

    // Forward declarations
    class Camera;

    // Renders a 2D stick-figure billboard for each remote player.
    // Follows the BlockHighlight pattern: Initialize() / Shutdown() / Render().
    class PlayerRenderer {
    public:
        PlayerRenderer();
        ~PlayerRenderer();

        // Allocate GPU resources (shader, dummy texture).  Call once.
        bool Initialize();

        // Release GPU resources.  Safe to call multiple times.
        void Shutdown();

        // Draw stick-figure billboards for all remote players.
        // Call after chunk rendering, before crosshair.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos,
                    const Client::RemotePlayerManager& remotePlayers);

    private:
        ShaderHandle  m_shader       = INVALID_SHADER;
        BufferHandle  m_vb           = INVALID_BUFFER;
        MeshHandle    m_mesh         = INVALID_MESH;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // Shader source (GLSL 330, used by GL backend)
        static const char* s_vertSource;
        static const char* s_fragSource;

        // Maximum vertices we can fit in the dynamic VB
        static constexpr size_t MAX_VERTICES = 4096;
    };

} // namespace Render
