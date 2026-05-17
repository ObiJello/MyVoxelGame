// File: src/client/renderer/entity/PlayerRenderer.hpp
#pragma once

#include "../backend/RenderTypes.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include <glm/glm.hpp>
#include <unordered_set>
#include <cstdint>

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

        // `partialTick` (range [0, 1]) is the sub-tick fraction = how far
        // through the current 50ms client tick the renderer is. Used to
        // interpolate each remote player's position/rotation between the
        // previous-tick snapshot and the current value, mirroring MC's
        // Entity.getPosition(partialTick) (Entity.java:1955-1960). Without
        // this we'd render the same position N times per tick, producing
        // visible 50ms-period stutter.
        // `skipIds` (optional) — player IDs to EXCLUDE from the bulk pass.
        // Portal-straddling players are excluded here and re-rendered
        // individually via RenderSingle with an entry-clip plane so the
        // half of the body that's "already through" the portal isn't
        // drawn twice. Pass nullptr to render everyone.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos,
                    const Client::RemotePlayerManager& remotePlayers,
                    float partialTick,
                    const std::unordered_set<uint32_t>* skipIds = nullptr);

        // Render a single arbitrary player (NOT in RemotePlayerManager).
        // Used by the portal see-through pass to draw the LOCAL player as a
        // stick figure when the player can see themselves through a portal —
        // the local player is normally invisible (it IS the camera). No
        // distance cull, no nametag, no chat bubble.
        //
        // `model` and `clipPlane` carry portal-ghost half-body rendering
        // state (see PortalGhostRenderer-style usage in PlatformMain): pass
        // identity matrix + zero plane for normal rendering. For ghost
        // rendering, pass the source→destination portal matrix M and the
        // destination portal's plane equation so only the "emerged" half
        // of the body is drawn (clipped against the destination wall).
        void RenderSingle(const glm::mat4& projection, const glm::mat4& view,
                          const glm::vec3& position,
                          float headYaw, float bodyYaw, float pitch,
                          bool isCrouching, uint8_t colorId,
                          const glm::mat4& model     = glm::mat4(1.0f),
                          const glm::vec4& clipPlane = glm::vec4(0.0f));

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
