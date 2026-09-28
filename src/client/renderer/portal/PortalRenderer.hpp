// File: src/client/renderer/portal/PortalRenderer.hpp
//
// Phase 4: PLACEHOLDER renderer for portals. Draws each active portal as a
// flat colored 1×2 oval (32-segment ellipse triangle fan) anchored to the
// wall surface, with a subtle pulse animation. NOT see-through yet — that's
// Phase 6 (single-pass stencil) / Phase 7 (recursive).
//
// The mesh + shader infrastructure built here is reused in Phase 5+ as the
// stencil-mask geometry for the see-through pass: same ellipse, drawn first
// with color writes off + stencil writes on, then re-rendered for depth refill
// after the scene re-render. So the work isn't throwaway.
//
// Initialization deliberately mirrors BlockHighlight's pattern: source-based
// shader compile (GL backend), no SPIR-V yet. Vulkan rendering of portals is
// deferred until Phase 5 lands the stencil plumbing — at that point both
// backends get .spv files.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "../backend/RenderTypes.hpp"
#include "../core/Camera.hpp"
#include "../core/Frustum.hpp"
#include <glm/glm.hpp>
#include <functional>
#include <vector>

namespace Render {

    class PortalRenderer {
    public:
        PortalRenderer();
        ~PortalRenderer();

        bool Initialize();
        void Shutdown();

        // Callback the see-through pass invokes once per recursion level.
        // Implementation should draw the scene (chunks, players, anything
        // else that should be visible THROUGH the portal) using the supplied
        // virtual camera + oblique projection. The portal renderer manages
        // stencil setup before/after the call — the callback just renders.
        using SceneRenderFn = std::function<void(const Camera& virtualCam,
                                                  const Frustum& virtualFrustum,
                                                  const glm::mat4& obliqueProjection)>;

        // Walk the global ClientPortalManager and draw every active portal.
        //
        // For an active pair (BOTH colors placed) the renderer does a true
        // recursive see-through pass (Phase 7):
        //   • Build a stack of (portal, virtualCamera) up to PORTAL_RECURSION_DEPTH
        //     levels deep, alternating between the two portals each level.
        //   • For each level (outer→inner):
        //       1. stencil-mark the level's source portal silhouette,
        //          gated by the previous level's stencil
        //       2. clear depth+color inside that silhouette
        //       3. invoke `renderScene(virtualCam, virtualFrustum, obliqueProj)`
        //          to fill the silhouette with that level's destination view
        //   • At the end, depth-refill the outermost silhouette so subsequent
        //     translucent draws composite correctly.
        //
        // For a half-placed pair (only one color so far) the orphan portal
        // falls back to a flat colored oval — no recursion, no callback fire.
        void Render(const glm::mat4& projectionMatrix,
                    const glm::mat4& viewMatrix,
                    const Camera& camera,
                    const Frustum& frustum,
                    float aspect,
                    float farPlane,
                    int8_t dimensionFilter,
                    const SceneRenderFn& renderScene,
                    const glm::dvec3* skipRimAt = nullptr);
        // Draw every portal regardless of the level it is in.
        static constexpr int8_t kAnyDimension = 127;

        // The leave capture's panorama faces (PlatformMain): each face is
        // drawn as off-axis tiles over several frames, so while they are
        // drawn the see-through view is built from the TILE's projection
        // (not a symmetric perspective of the window's aspect) and the rim
        // animation clock is held at one instant, so a rim that crosses a
        // tile seam matches on both sides. Null / negative = off.
        void SetCaptureOverrides(const glm::mat4* baseProjection, double frozenTimeSeconds) {
            m_captureProjValid = baseProjection != nullptr;
            if (baseProjection) m_captureProj = *baseProjection;
            m_captureTime = frozenTimeSeconds;
        }
        void ClearCaptureOverrides() { m_captureProjValid = false; m_captureTime = -1.0; }

        // ── Culling ──────────────────────────────────────────────────────
        // Render skips a portal (its see-through level pass AND its rim)
        // that cannot show in the view: the camera behind its plane, its
        // oval's box outside the view frustum, or — with an occlusion
        // source — every section its box touches missing from that view's
        // section list (the occlusion BFS did not reach it). Sections the
        // list never holds (all-air, unloaded) count as visible, so a
        // portal even partly in view is never skipped.
        enum class Occlusion : uint8_t {
            None,          // frustum + back-face only
            MainView,      // the frame's main view list (ChunkRenderer::IsMainViewSection)
            CurrentView,   // the list of the chunk pass that ran last (a portal view's own)
            Snapshot,      // what SnapshotVisibility recorded
        };
        // Returns the previous source, for the caller to restore.
        Occlusion SetOcclusion(Occlusion occlusion) {
            const Occlusion previous = m_occlusion;
            m_occlusion = occlusion;
            return previous;
        }
        // Record, from the CURRENT view's section list, which portals it can
        // show — for a later Render with Occlusion::Snapshot after other
        // chunk passes (the panorama's immersive pass) replaced the list.
        void SnapshotVisibility();

    private:
        bool      m_captureProjValid = false;
        glm::mat4 m_captureProj{1.0f};
        double    m_captureTime = -1.0;
        Occlusion m_occlusion = Occlusion::None;
        std::vector<uint64_t> m_snapshotVisible;   // (gunId << 1) | color, sorted
        // Resources are created once and held for the renderer's lifetime.
        BufferHandle m_vb            = INVALID_BUFFER;
        BufferHandle m_ib            = INVALID_BUFFER;
        MeshHandle   m_mesh          = INVALID_MESH;
        ShaderHandle m_shader        = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE; // Vulkan layout requires one
        uint32_t     m_indexCount    = 0;
        bool         m_initialized   = false;


        // Refraction sub-pass resources (item #9). Captures the just-
        // rendered see-through view into m_sceneSnapshot, then re-renders
        // the silhouette via m_refractionShader sampling the snapshot at
        // rim-distorted UVs. Snapshot is resized lazily when the
        // framebuffer dimensions change.
        ShaderHandle  m_refractionShader = INVALID_SHADER;
        TextureHandle m_sceneSnapshot    = INVALID_TEXTURE;
        int           m_snapshotWidth    = 0;
        int           m_snapshotHeight   = 0;

        // Portal textures extracted from Portal 1's VPK (see
        // assets/textures/portal/). Drives the direct port of Valve's
        // portal_refract_ps2x.fxc Stage 2 algorithm.
        //   m_noiseTexture   : 256×256 noise texture used for the rim's
        //                      flow-distortion (Portal's
        //                      models/portals/noise-blur-256x256.vtf).
        //                      Wrap mode: Repeat (texture scrolls).
        //   m_blueColorRamp  : 256×1 1D color LUT for blue rim
        //                      (portal-blue-color.vtf). ClampToEdge.
        //   m_orangeColorRamp: 256×1 1D color LUT for orange rim
        //                      (portal-orange-color.vtf). ClampToEdge.
        //   m_maskTexture    : kept for the SeeThroughPass refraction
        //                      sub-pass (portal_mask.vtf — silhouette).
        //                      NOT used by the rim shader (Valve derives
        //                      silhouette from radial distance).
        TextureHandle m_noiseTexture    = INVALID_TEXTURE;
        TextureHandle m_maskTexture     = INVALID_TEXTURE;
        TextureHandle m_blueColorRamp   = INVALID_TEXTURE;
        TextureHandle m_orangeColorRamp = INVALID_TEXTURE;

        // Inline shader sources — keep alongside the GLSL conventions used by
        // the rest of the codebase (BlockHighlight, etc.).
        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;
        static const char* refractionFragmentSource;
    };

    extern PortalRenderer g_portalRenderer;

} // namespace Render

#endif // ENABLE_PORTAL_GUN
