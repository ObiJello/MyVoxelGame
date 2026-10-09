// File: src/client/renderer/entity/PlayerRenderer.hpp
#pragma once

#include "../backend/RenderTypes.hpp"
#include "EntityFrame.hpp"
#include "StickFigureGeometry.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include <glm/glm.hpp>
#include <unordered_map>
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
        // `position`, `model` and `clipPlane` are WORLD space, in double: the
        // body is built in the view's render space (RenderOrigin.hpp), the
        // model is bridged into it in double, the plane re-expressed there.
        void RenderSingle(const glm::mat4& projection, const glm::mat4& view,
                          const glm::dvec3& position,
                          float headYaw, float bodyYaw, float pitch,
                          bool isCrouching, uint8_t colorId,
                          const glm::dmat4& model     = glm::dmat4(1.0),
                          const glm::dvec4& clipPlane = glm::dvec4(0.0),
                          float deathFlipDeg = 0.0f,
                          bool glowing = false,
                          bool drawBody = true,
                          bool isSitting = false,
                          bool spectatorHead = false,
                          float fallFlyTicks = 0.0f,
                          float spinAttackAgeTicks = -1.0f,
                          uint32_t subjectId = kLocalPlayer);
        // `subjectId`: whose look the figure takes — kLocalPlayer (the
        // launcher's choice: colour, the painted figure, or a drawn figure
        // in its place), or a remote player's id (their painted or drawn
        // figure from PlayerAppearanceS2C; the colour still comes from
        // `colorId`). A subject whose look is a
        // Minecraft skin draws nothing here — the player model is
        // MobRenderer::RenderPlayerSkins'.
        static constexpr uint32_t kLocalPlayer = 0xFFFFFFFFu;
        // `fallFlyTicks` (> 0 while gliding, partial tick included): the
        // elytra glide tip (MC setupRotations' fall-flying branch), as the
        // bulk pass gives remote players.
        // `spinAttackAgeTicks` (>= 0 while riptiding: the body's age in
        // ticks, partial included): the spin along the look.
        // `spectatorHead`: a spectator's body — the translucent head alone
        // (MC PlayerModel with isSpectator, drawn with forceTransparent).
        // `glowing`: also into the GLOWING outline pass (EntityOutline.hpp)
        // while the main view is collecting; `drawBody` false with it: the
        // outline alone (an INVISIBLE glowing body, MC's outline render type).
        // `isSitting`: the seated pose (a player on a cushion).

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

        // Per frame, from the local player: a spectator sees INVISIBLE
        // players translucent (MC Entity.isInvisibleTo), and while holding
        // key.spectatorOutlines every non-spectator player glows
        // (Minecraft.shouldEntityAppearGlowing).
        void SetViewerSeesInvisible(bool sees) { m_viewerSeesInvisible = sees; }
        void SetOutlinePlayers(bool outline)   { m_outlinePlayers = outline; }

    private:
        Tally m_tally;
        // Upload this call's m_triVerts / m_lineVerts into the frame's set
        // and draw both passes. `clipPlane` is the portal ghost half-body
        // plane (zero = off); the vertices are already in render space, as
        // is `cameraPos`. `glowing`: the figures also go to the GLOWING
        // outline pass (EntityOutline.hpp).
        // `drawBody` false: outline only (an INVISIBLE glowing body).
        // `translucent`: alpha-blended (the spectator's translucent figures).
        void SubmitFigures(const glm::mat4& mvp, const glm::vec3& cameraPos,
                           const glm::vec4& clipPlane, bool glowing, bool drawBody = true,
                           bool translucent = false);

        // The stick figures (shaders/stick_figure.*): lit and fogged like
        // every entity (EntityEnvironment.hpp).
        ShaderHandle  m_shader       = INVALID_SHADER;
        // The chat bubbles: flat screen-space colour (player_billboard.*).
        ShaderHandle  m_bubbleShader = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // One streaming set per frame in flight, cycled per FRAME, every call in a frame
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
        FrameBuffers m_frames[EntityFrame::kMaxSlots];   // EntityFrame::Slots() of them exist
        EntityFrame::Cursor m_frameCursor;
        size_t m_lineCursor = 0;   // strip vertices written this frame
        size_t m_triCursor  = 0;   // triangle vertices written this frame

        // Build scratch, reused across calls so a frame allocates nothing.
        std::vector<StickVertex> m_lineVerts;
        std::vector<StickVertex> m_triVerts;
        std::vector<StickVertex> m_stripVerts;
        // Render's GLOWING figures, submitted after the rest: drawn and
        // outlined, and (INVISIBLE ones) outlined only.
        std::vector<StickVertex> m_glowLineVerts;
        std::vector<StickVertex> m_glowTriVerts;
        std::vector<StickVertex> m_outlineOnlyLineVerts;
        std::vector<StickVertex> m_outlineOnlyTriVerts;
        // Render's translucent figures (spectator heads; INVISIBLE bodies a
        // spectator viewer sees), and the dropped lines of a head-only build.
        std::vector<StickVertex> m_translucentLineVerts;
        std::vector<StickVertex> m_translucentTriVerts;
        std::vector<StickVertex> m_scratchLineVerts;
        bool m_viewerSeesInvisible = false;
        bool m_outlinePlayers      = false;

        // Drawn figures (Game::StickFigureDrawing): each subject's strokes,
        // flattened and layered once per look (PlayerSkins' revision), placed
        // per frame and widened toward the camera with the limbs.
        struct DrawingCacheEntry {
            uint64_t    revision = 0;
            DrawingMesh mesh;
            DrawingMesh head;          // the spectator's floating head, meshed on first use
            bool        headBuilt = false;
        };
        std::unordered_map<uint32_t, DrawingCacheEntry> m_drawingCache;
        // The subject's mesh (kLocalPlayer or a remote id), rebuilt when its
        // look changed — `headOnly`: the strokes from the neck up; null when the
        // subject is not a drawn figure (it is the stick figure, or a skin).
        const DrawingMesh* DrawingMeshFor(uint32_t subjectId, bool headOnly = false);
        // Drops the meshes of subjects that are no longer drawn figures
        // (left, changed look). Once per bulk pass.
        void PruneDrawingCache();
        // Appends `mesh` at a body (feet, body yaw, the sneak) to `out` — the
        // figure's line list — in `shade`'s palette. Returns where it starts.
        template <class Shade>
        size_t AppendDrawing(std::vector<StickVertex>& out, const DrawingMesh& mesh, const glm::vec3& feet,
                             float bodyYaw, bool crouching, Shade&& shade);
        // The last bulk pass's partial tick: RenderSingle has none of its own
        // and a drawn figure's swim tilt lerps by it.
        float m_partialTick = 1.0f;

        static const char* s_vertSource;
        static const char* s_fragSource;

        // Each line segment becomes a 6-vert camera-facing thick triangle
        // strip: a stick figure's limbs are ~6 segments, ~36 vertices. The
        // set also carries the drawn figures' strokes — a ribbon and a round
        // joint per point, about 27 vertices at the limbs' width and 60 at
        // the largest brush: a few thousand for an ordinary drawing, and
        // StickFigureDrawing::kMaxPoints (4,096) of the largest brush just
        // fits on its own. 6 MB a set, shared by every call in a frame; a
        // frame that overflows draws the strips that fit.
        static constexpr size_t MAX_VERTICES = 262144;
        // The triangle set: the head rings and back-of-head discs, ~640
        // vertices a figure — about 100 figures a frame. A frame that
        // overflows draws the triangles that fit.
        static constexpr size_t MAX_TRI_VERTICES = 65536;
    };

} // namespace Render
