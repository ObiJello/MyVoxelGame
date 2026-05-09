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

        // Render chat bubbles above remote players (screen-space billboarded)
        void RenderChatBubbles(const glm::mat4& projection, const glm::mat4& view,
                               const Client::RemotePlayerManager& remotePlayers,
                               int fbWidth, int fbHeight);

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

        // Each line segment becomes a 6-vert camera-facing thick triangle strip.
        // Per player: head circle (64) + smile (32) + eyes (2) + body/limbs (~6) ≈
        // 100 segments × 6 ≈ 620 verts/player. 65 536 / 620 ≈ 105 players concurrent
        // before this buffer fills.
        static constexpr size_t MAX_VERTICES = 65536;
    };

} // namespace Render
