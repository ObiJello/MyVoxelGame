// File: src/client/renderer/entity/PlayerRenderer.hpp
#pragma once

#include "../backend/RenderTypes.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include <glm/glm.hpp>

namespace Render {

    class Camera;

    // Renders stick-figure models for remote players.
    // Two draw passes: filled triangle disc for back-of-head (GPU face-culled),
    // then lines for body/limbs/front face features.
    class PlayerRenderer {
    public:
        PlayerRenderer();
        ~PlayerRenderer();

        bool Initialize();
        void Shutdown();

        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos,
                    const Client::RemotePlayerManager& remotePlayers);

    private:
        ShaderHandle  m_shader       = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // Line geometry (body, limbs, head outline, face features)
        BufferHandle  m_lineVB   = INVALID_BUFFER;
        MeshHandle    m_lineMesh = INVALID_MESH;

        // Triangle geometry (filled back-of-head disc)
        BufferHandle  m_triVB    = INVALID_BUFFER;
        MeshHandle    m_triMesh  = INVALID_MESH;

        static const char* s_vertSource;
        static const char* s_fragSource;

        static constexpr size_t MAX_VERTICES = 8192;
    };

} // namespace Render
