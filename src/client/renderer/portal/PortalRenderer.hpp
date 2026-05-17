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
                    const SceneRenderFn& renderScene);

    private:
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
