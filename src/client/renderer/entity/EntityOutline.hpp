// File: src/client/renderer/entity/EntityOutline.hpp
//
// MC's GLOWING outline — the entity_outline target and post chain.
//
// MC (LevelRenderer + RenderPipelines.OUTLINE_* + post_effect/
// entity_outline.json):
//   1. Every entity that appears glowing (Minecraft.shouldEntityAppearGlowing:
//      isCurrentlyGlowing — the GLOWING effect or the glowing tag) is drawn a
//      second time through RenderTypes.outline into the "Entity Outline"
//      target: RGBA8, cleared to transparent black, NO depth test, so a
//      glowing mob behind a wall still lands in it. rendertype_outline.fsh
//      writes the team colour (white without a team) at alpha 1 wherever the
//      entity's texture is not fully transparent. An INVISIBLE glowing
//      entity still goes in (its body render type becomes the outline alone).
//   2. The post chain: entity_sobel (target -> swap) marks the silhouette's
//      edge one texel each side; entity_outline_box_blur horizontally
//      (swap -> target) and vertically (target -> swap), both sampled
//      bilinear with Radius 2; then a blit (swap -> target).
//   3. GameRenderer.render, after renderLevel (the hand included):
//      blitEntityOutline lays the target over the main image with
//      BlendFunction.ENTITY_OUTLINE_BLIT (SRC_ALPHA, ONE_MINUS_SRC_ALPHA),
//      depth ignored — which is what shows the outline through walls.
//
// Here the entity renderers keep drawing as they do and, while the MAIN
// view's entities are being drawn (SetCollecting), hand this module the
// index / vertex ranges of their glowing entities' geometry — the ranges
// they just uploaded, with the matrix they drew them with. Composite, called
// once per frame after the viewmodel, redraws those ranges flat into the
// outline target and runs the post chain on both backends (GL FBOs,
// VKBackend's render targets). The chain's final blit (swap -> target) is
// folded into the composite, which reads swap directly: the pixels are the
// same.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Render {

    class EntityOutline {
    public:
        static EntityOutline& Get();

        bool Initialize();
        void Shutdown();

        // Once per frame, before the first entity renderer runs: forget the
        // previous frame's glowing entities.
        void BeginFrame();
        // True while the MAIN view's entities are drawn — the only view MC
        // outlines (portal views and panorama captures are not collected).
        void SetCollecting(bool on) { m_collecting = on; }
        bool Collecting() const { return m_collecting && m_ready; }

        // MC Entity.getTeamColor with no team.
        static constexpr uint32_t kDefaultTeamColor = 0xFFFFFFu;

        // A glowing entity's geometry, already uploaded by its renderer for
        // this frame: [first, first + count) of `mesh`, drawn with `mvp`
        // (GL-style; the renderer's own uMVP) and `texture` (its alpha
        // decides the silhouette, as rendertype_outline.fsh does).
        void SubmitIndexed(MeshHandle mesh, uint32_t firstIndex, uint32_t indexCount,
                           TextureHandle texture, const glm::mat4& mvp,
                           uint32_t teamColor = kDefaultTeamColor);
        void SubmitArrays(MeshHandle mesh, uint32_t firstVertex, uint32_t vertexCount,
                          TextureHandle texture, const glm::mat4& mvp,
                          uint32_t teamColor = kDefaultTeamColor);

        // MC featureFrame.hasAnyOutline: something to outline this frame.
        bool HasAny() const { return !m_requests.empty(); }

        // The silhouettes, the post chain and the blend over the frame (MC
        // executeOutline + the entity_outline chain + blitEntityOutline).
        // Leaves the default framebuffer bound with the full-frame viewport.
        // On Vulkan this interrupts the frame's render pass, which discards
        // its depth (VKBackend's render-target note) — call it once the
        // world and the hand are drawn, before the GUI.
        void Composite(int framebufferWidth, int framebufferHeight);

    private:
        EntityOutline() = default;

        struct Request {
            MeshHandle    mesh = INVALID_MESH;
            uint32_t      first = 0;
            uint32_t      count = 0;
            bool          indexed = true;
            TextureHandle texture = INVALID_TEXTURE;
            glm::mat4     mvp{1.0f};
            glm::vec3     color{1.0f};
        };
        std::vector<Request> m_requests;
        bool m_collecting = false;
        bool m_ready = false;
        bool m_initTried = false;

        ShaderHandle m_silhouetteShader = INVALID_SHADER;
        ShaderHandle m_sobelShader = INVALID_SHADER;
        ShaderHandle m_blurShader = INVALID_SHADER;
        ShaderHandle m_blitShader = INVALID_SHADER;

        // MC's "Entity Outline" target and the chain's "swap".
        RenderTargetHandle m_outlineTarget = INVALID_RENDER_TARGET;
        RenderTargetHandle m_swapTarget = INVALID_RENDER_TARGET;
        int m_targetWidth = 0;
        int m_targetHeight = 0;

        // The full-screen quad (six vertices, the block vertex layout).
        BufferHandle m_quadVB = INVALID_BUFFER;
        MeshHandle   m_quadMesh = INVALID_MESH;

        // Frees everything; a failed Initialize stays failed until Shutdown.
        void ReleaseResources();
        bool EnsureTargets(int width, int height);
        // One post pass: `input`'s colour through `shader` into `output`.
        void PostPass(ShaderHandle shader, RenderTargetHandle input, RenderTargetHandle output,
                      const glm::vec2& blurDir);
    };

} // namespace Render
