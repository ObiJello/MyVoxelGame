// File: src/client/renderer/post/ImprovedTransparency.cpp
#include "ImprovedTransparency.hpp"

#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

namespace Render {

    namespace {
        // The full-screen quad in the block vertex layout (24 bytes), as the
        // post chains draw it (PostChain.cpp).
        struct QuadVert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(QuadVert) == 24, "must match GetBlockVertexLayout");
    } // namespace

    ImprovedTransparency& ImprovedTransparency::Get() {
        static ImprovedTransparency s_instance;
        return s_instance;
    }

    void ImprovedTransparency::BeginFrame(bool enabled, int framebufferWidth, int framebufferHeight) {
        m_active = false;
        for (auto& draw : m_draws) draw = nullptr;
        if (!g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;

        // Only a CHANGE reaches the backend: with the option off, nothing
        // here touches it at all.
        const bool wantPreserve = enabled && !m_failed;
        if (wantPreserve != m_preserveRequested) {
            m_preserveRequested = wantPreserve;
            b.SetFrameDepthPreserved(wantPreserve);
        }
        if (!enabled) {
            if (m_composite != INVALID_SHADER || m_quadMesh != INVALID_MESH) Release();
            m_failed = false;   // switching it back on tries again
            return;
        }
        if (m_failed || framebufferWidth <= 0 || framebufferHeight <= 0) return;
        if (!b.FrameDepthPreserved()) return;   // Vulkan: from the next frame
        if (!EnsureResources(framebufferWidth, framebufferHeight)) {
            Release();
            if (!m_failed) {
                Log::Warning("[ImprovedTransparency] OIT targets or shaders unavailable - "
                             "disabled for this session, drawing the classic way");
            }
            m_failed = true;
            m_preserveRequested = false;
            b.SetFrameDepthPreserved(false);
            return;
        }
        m_active = true;
    }

    bool ImprovedTransparency::EnsureResources(int width, int height) {
        RenderBackend& b = *g_renderBackend;
        if (!b.OitEnsureTargets(width, height)) return false;
        m_width = width;
        m_height = height;
        auto shader = [&b](ShaderHandle& h, const char* frag) {
            if (h == INVALID_SHADER) h = b.CreateOitShaderFromFiles("shaders/post_screenquad.vert", frag);
            return h != INVALID_SHADER;
        };
        if (!shader(m_composite, "shaders/oit_composite.frag") ||
            !shader(m_cull, "shaders/oit_depth_bounds_cull.frag") ||
            !shader(m_blit, "shaders/oit_blit.frag") ||
            !shader(m_blitDepth, "shaders/oit_blit_depth.frag")) {
            return false;
        }
        if (m_quadMesh == INVALID_MESH) {
            const QuadVert quad[6] = {
                {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
                { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255},
                { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
                {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
                { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
                {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
            };
            m_quadVB = b.CreateBuffer(BufferUsage::Vertex, sizeof(quad), quad, BufferAccess::Static);
            if (m_quadVB != INVALID_BUFFER) m_quadMesh = b.CreateMesh(m_quadVB, INVALID_BUFFER, GetBlockVertexLayout());
            if (m_quadMesh == INVALID_MESH) return false;
        }
        return true;
    }

    void ImprovedTransparency::Release() {
        m_active = false;
        for (auto& draw : m_draws) draw = nullptr;
        ReleaseChunksTranslucentForOit();
        if (!g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;
        for (ShaderHandle* h : {&m_composite, &m_cull, &m_blit, &m_blitDepth}) {
            if (*h != INVALID_SHADER) b.DestroyShader(*h);
            *h = INVALID_SHADER;
        }
        if (m_quadMesh != INVALID_MESH) { b.DestroyMesh(m_quadMesh); m_quadMesh = INVALID_MESH; }
        if (m_quadVB != INVALID_BUFFER) { b.DestroyBuffer(m_quadVB); m_quadVB = INVALID_BUFFER; }
        b.OitDestroyTargets();
        m_width = m_height = 0;
    }

    bool ImprovedTransparency::Defer(Feature feature, std::function<void()> draw) {
        if (!m_active || feature == Feature::Count) return false;
        m_draws[static_cast<size_t>(feature)] = std::move(draw);
        return true;
    }

    void ImprovedTransparency::DrawQuad() {
        g_renderBackend->DrawArrays(m_quadMesh, 6, 0);
        g_renderBackend->UnbindMesh();
    }

    void ImprovedTransparency::RunFeatures(bool clouds) {
        // Non-cloud features in MC's order (terrain, then the features and
        // the weather — the order does not matter to MAX or ADD blending).
        if (clouds) {
            if (auto& draw = m_draws[static_cast<size_t>(Feature::Clouds)]) draw();
            return;
        }
        for (Feature f : {Feature::Terrain, Feature::Items, Feature::Particles, Feature::Weather}) {
            if (auto& draw = m_draws[static_cast<size_t>(f)]) draw();
        }
    }

    void ImprovedTransparency::EndFrameWork() {
        for (auto& draw : m_draws) draw = nullptr;
        ReleaseChunksTranslucentForOit();
    }

    void ImprovedTransparency::Resolve(const glm::mat4& projection) {
        if (!m_active) return;
        m_active = false;
        bool any = false;
        for (const auto& draw : m_draws) any = any || static_cast<bool>(draw);
        if (!any || !g_renderBackend) { EndFrameWork(); return; }
        PROFILE_ZONE_N("ImprovedTransparency.Oit");
        RenderBackend& b = *g_renderBackend;
        using Stage = RenderBackend::OitStage;
        using Pass = RenderBackend::OitPass;
        using Image = RenderBackend::OitImage;

        // MC deviceToLinearDepth's ProjMat[2][2] and ProjMat[3][2].
        const glm::vec4 proj(projection[2][2], projection[3][2], 0.0f, 0.0f);
        const bool clouds = static_cast<bool>(m_draws[static_cast<size_t>(Feature::Clouds)]);

        auto fail = [&](const char* what) {
            b.SetOitStage(Stage::None, proj, false);
            b.OitEndPass();
            if (++m_skippedFrames == 1 || m_skippedFrames % 600 == 0) {
                Log::Warning("[ImprovedTransparency] %s - this frame's translucency skipped (%u so far)",
                             what, m_skippedFrames);
            }
            EndFrameWork();
            PipelineState def;
            b.SetPipelineState(def);
        };

        // Full-screen helper passes: no depth test unless asked, no blend.
        auto quadState = [&b](bool depthTest, CompareOp op, bool depthWrite, bool colorWrite) {
            PipelineState s;
            s.depthTestEnabled  = depthTest;
            s.depthCompareOp    = op;
            s.depthWriteEnabled = depthWrite;
            s.blendEnabled      = false;
            s.colorWriteEnabled = colorWrite;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            b.SetPipelineState(s);
        };

        const Stage stages[3] = {Stage::DepthBounds, Stage::Transmittance, Stage::Accumulate};
        const Pass featurePasses[3] = {Pass::DepthBounds, Pass::Transmittance, Pass::Accumulate};
        const Pass cloudPasses[3] = {Pass::CloudDepthBounds, Pass::CloudTransmittance, Pass::CloudAccumulate};
        for (int i = 0; i < 3; ++i) {
            const Stage stage = stages[i];
            // The features (always opened: its clear starts the stage's
            // targets from MC's clear values, clouds or not).
            if (!b.OitBeginPass(featurePasses[i], /*clearColor=*/true)) { fail("an OIT pass could not open"); return; }
            b.SetOitStage(stage, proj, false);
            RunFeatures(false);
            b.SetOitStage(Stage::None, proj, false);
            b.OitEndPass();

            if (stage == Stage::DepthBounds) {
                // MC executeDepthBoundsCull: the bounds copied into the culled
                // target, then the far bound pulled in to the nearest opaque
                // OIT surface, whose depth goes into the depth buffer.
                if (!b.OitBeginPass(Pass::DepthBoundsCull, /*clearColor=*/false)) { fail("the OIT cull pass could not open"); return; }
                quadState(false, CompareOp::Always, false, true);
                b.BindShader(m_blit);
                b.BindTexture(b.OitTexture(Image::DepthBounds), 0);
                b.SetUniformInt(m_blit, "Sampler0", 0);
                DrawQuad();
                quadState(true, CompareOp::LessEqual, true, true);
                b.BindShader(m_cull);
                b.SetUniformVec4(m_cull, "OitProjParams", proj);
                DrawQuad();
                b.OitEndPass();
            }

            if (clouds) {
                // MC CloudRenderer.renderOit: the clouds' own depth, the
                // frame's blitted in before their depth bounds (which write
                // it), so only the nearest cloud surface counts afterwards.
                if (!b.OitBeginPass(cloudPasses[i], /*clearColor=*/false)) { fail("an OIT cloud pass could not open"); return; }
                if (stage == Stage::DepthBounds) {
                    quadState(true, CompareOp::Always, true, false);
                    b.BindShader(m_blitDepth);
                    b.BindTexture(b.OitTexture(Image::FrameDepth), 0);
                    b.SetUniformInt(m_blitDepth, "Sampler0", 0);
                    DrawQuad();
                }
                b.SetOitStage(stage, proj, /*depthBoundsWriteDepth=*/stage == Stage::DepthBounds);
                RunFeatures(true);
                b.SetOitStage(Stage::None, proj, false);
                b.OitEndPass();
            }
        }

        // MC's OIT composite: premultiplied over the frame (TRANSLUCENT_
        // PREMULTIPLIED_ALPHA), the nearest translucent depth written.
        {
            PROFILE_ZONE_N("ImprovedTransparency.Composite");
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthCompareOp    = CompareOp::Always;
            s.depthWriteEnabled = true;
            s.blendEnabled      = true;
            s.srcBlendFactor    = BlendFactor::One;
            s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            b.SetViewport(0, 0, m_width, m_height);
            b.SetPipelineState(s);
            b.BindShader(m_composite);
            b.BindTexture(b.OitTexture(Image::Accumulate), 0);
            b.SetUniformInt(m_composite, "Sampler0", 0);
            b.SetUniformVec4(m_composite, "OitProjParams", proj);
            DrawQuad();
        }

        EndFrameWork();
        PipelineState def;
        b.SetPipelineState(def);
    }

} // namespace Render
