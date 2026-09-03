// File: src/client/renderer/entity/PlayerRenderer.hpp
#pragma once

#include "../backend/RenderTypes.hpp"
#include "EntityFrame.hpp"
#include "StickFigureGeometry.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include <glm/glm.hpp>
#include <unordered_set>
#include <cstdint>
#include <vector>

struct Frustum;

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
        // `frustum` is this view's (main or portal recursion) — MC
        // extractVisibleEntities' shouldRender AABB test plus the visible-
        // section gate; see EntityCulling.hpp.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, const Frustum& frustum,
                    const Client::RemotePlayerManager& remotePlayers,
                    float partialTick,
                    const std::unordered_set<uint32_t>* skipIds = nullptr,
                    const glm::vec4& clipPlane = glm::vec4(0.0f));

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
        // `deathFlipDeg` is MC LivingEntityRenderer.setupRotations' topple, in
        // degrees (MobRenderer::DeathFlipDegrees) — the corpse falling over.
        void RenderSingle(const glm::mat4& projection, const glm::mat4& view,
                          const glm::vec3& position,
                          float headYaw, float bodyYaw, float pitch,
                          bool isCrouching, uint8_t colorId,
                          const glm::mat4& model     = glm::mat4(1.0f),
                          const glm::vec4& clipPlane = glm::vec4(0.0f),
                          float deathFlipDeg = 0.0f);

        // Render chat bubbles above remote players (screen-space billboarded)
        void RenderChatBubbles(const glm::mat4& projection, const glm::mat4& view,
                               const Client::RemotePlayerManager& remotePlayers,
                               int fbWidth, int fbHeight);

        // What the last Render call did, for OBEY_PORTAL_DIAG: players in
        // the bound level, and how many of those each gate dropped.
        struct Tally {
            int inLevel = 0, drawn = 0, cullDistance = 0, cullFrustum = 0, cullCrossing = 0;
        };
        const Tally& LastTally() const { return m_tally; }

    private:
        Tally m_tally;
        // Upload this call's m_triVerts / m_lineVerts into the frame's set
        // and draw both passes. `clipPlane` is the portal ghost half-body
        // plane (zero = off); the vertices are already in world space.
        void SubmitFigures(const glm::mat4& mvp, const glm::vec3& cameraPos,
                           const glm::vec4& clipPlane);

        ShaderHandle  m_shader       = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // Two streaming sets alternated per FRAME, every call in a frame
        // (the bulk pass, each RenderSingle ghost, the portal pass's repeat
        // of all of them) appending at a cursor — the scheme EntityFrame.hpp
        // describes. One set rewritten per call was a Vulkan hazard: draws
        // run at submit, so every ghost drawn this frame used to read the
        // LAST call's vertices.
        struct FrameBuffers {
            // Line geometry (body, limbs, head outline, face features), as
            // camera-facing thick strips.
            BufferHandle lineVB   = INVALID_BUFFER;
            MeshHandle   lineMesh = INVALID_MESH;
            // Triangle geometry (head ring + filled back-of-head disc).
            BufferHandle triVB    = INVALID_BUFFER;
            MeshHandle   triMesh  = INVALID_MESH;
        };
        FrameBuffers m_frames[2];
        EntityFrame::Cursor m_frameCursor;
        size_t m_lineCursor = 0;   // strip vertices written this frame
        size_t m_triCursor  = 0;   // triangle vertices written this frame

        // Build scratch, reused across calls so a frame allocates nothing.
        std::vector<StickVertex> m_lineVerts;
        std::vector<StickVertex> m_triVerts;
        std::vector<StickVertex> m_stripVerts;

        static const char* s_vertSource;
        static const char* s_fragSource;

        // Each line segment becomes a 6-vert camera-facing thick triangle strip.
        // Per player: head circle (64) + smile (32) + eyes (2) + body/limbs (~6) ≈
        // 100 segments × 6 ≈ 620 verts/player. 65 536 / 620 ≈ 105 players concurrent
        // before this buffer fills. Per set, shared by every call in a frame.
        static constexpr size_t MAX_VERTICES = 65536;
    };

} // namespace Render
