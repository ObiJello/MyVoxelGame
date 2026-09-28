// File: src/client/renderer/post/ImprovedTransparency.hpp
//
// MC 26.3's "Improved Transparency" video option: wavelet order-independent
// transparency (LevelRenderer.executeOit, client/renderer/oit/*, shaders/
// include/oit*.glsl, core/oit_composite.fsh, core/oit_depth_bounds_cull.fsh).
//
// Every translucent feature — here translucent terrain, dropped items,
// particles, weather, clouds — is drawn three times instead of blended once:
//   DEPTH_BOUNDS   the nearest / farthest translucent depth per pixel, and
//                  the nearest fully opaque one (MAX-blended, RGBA32F);
//   (cull)         the far bound pulled in to that opaque surface, whose depth
//                  goes into the depth buffer (oit_depth_bounds_cull);
//   TRANSMITTANCE  the absorbance along the view ray as wavelet coefficients
//                  over the bounded depth range (two RGBA16F targets, added);
//   ACCUMULATE     each fragment's premultiplied colour times the
//                  transmittance in front of it (RGBA16F, added);
// then one composite renormalises the accumulation to the total coverage,
// blends it over the frame and writes the nearest translucent depth. The
// clouds do each stage in passes of their own against their own depth (the
// frame's, blitted in during their depth bounds), as MC's CloudRenderer.
// renderOit does. The per-stage shaders are the participating shaders'
// OIT variants (RenderBackend::SetOitStage; shaders/oit_lib.glsl).
//
// Where: the features are not drawn at their usual places in the frame but
// handed over (Defer) and drawn at Resolve — after the weather, before the
// hand, where MC's main pass ends — so the three stages see all of them.
// Translucent terrain is captured where it would have been drawn and
// replayed (ChunkRenderer::CaptureDeferredTranslucentForOit), because the
// portal views in between rebuild the renderer's view.
//
// OFF (the default): Defer returns false and every feature draws exactly
// where and how it always did; no target, shader, pass or backend mode
// exists. ON makes the backend's targets (one set per frame slot on Vulkan,
// which must also keep the frame's depth — SetFrameDepthPreserved) and
// frees them the frame it is turned off.
//
// Only the player's own view: portal views, the panorama capture and a
// shader pack's pipeline draw their translucency the classic way (a shader
// pack turns the option off, as OptiFine does). The portal gun's additive
// sparks stay classic too.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>

namespace Render {

    class ImprovedTransparency {
    public:
        enum class Feature : uint8_t { Terrain = 0, Items, Particles, Weather, Clouds, Count };

        static ImprovedTransparency& Get();

        // Once per frame, after the backend's BeginFrame and before the level
        // is drawn. `enabled`: the video option, and no shader pack. (Vulkan
        // keeps the frame's depth from the NEXT BeginFrame after it is asked
        // to — the first frame after the option comes on draws classically.)
        void BeginFrame(bool enabled, int framebufferWidth, int framebufferHeight);
        bool Active() const { return m_active; }

        // At a feature's usual place: when active, keeps `draw` for Resolve
        // and returns true (the caller draws nothing); otherwise false.
        bool Defer(Feature feature, std::function<void()> draw);

        // MC executeOit + the composite. After the last feature's usual
        // place, before the hand. `projection` is the view's.
        void Resolve(const glm::mat4& projection);

        // Frees everything (also what turning the option off does).
        void Release();

    private:
        ImprovedTransparency() = default;

        bool EnsureResources(int width, int height);
        void DrawQuad();
        void RunFeatures(bool clouds);
        void EndFrameWork();

        static constexpr size_t kFeatures = static_cast<size_t>(Feature::Count);
        std::array<std::function<void()>, kFeatures> m_draws{};

        ShaderHandle m_composite = INVALID_SHADER;   // oit_composite
        ShaderHandle m_cull      = INVALID_SHADER;   // oit_depth_bounds_cull
        ShaderHandle m_blit      = INVALID_SHADER;   // depth bounds -> culled
        ShaderHandle m_blitDepth = INVALID_SHADER;   // frame depth -> clouds' depth
        BufferHandle m_quadVB    = INVALID_BUFFER;
        MeshHandle   m_quadMesh  = INVALID_MESH;

        int  m_width = 0, m_height = 0;
        bool m_active = false;
        bool m_failed = false;              // gave up for the session (logged once)
        bool m_preserveRequested = false;
        uint32_t m_skippedFrames = 0;
    };

} // namespace Render
