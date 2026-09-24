// File: src/client/renderer/entity/EntityOutline.cpp
#include "EntityOutline.hpp"

#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <array>
#include <cstring>

namespace Render {

    namespace {
        // The block vertex layout (24 bytes): NDC position, the GL-convention
        // texture coordinate, an unused colour.
        struct QuadVert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(QuadVert) == 24, "must match GetBlockVertexLayout");

        glm::vec3 TeamColorRGB(uint32_t rgb) {
            return glm::vec3(static_cast<float>((rgb >> 16) & 0xFF),
                             static_cast<float>((rgb >> 8) & 0xFF),
                             static_cast<float>(rgb & 0xFF)) / 255.0f;
        }
    } // namespace

    EntityOutline& EntityOutline::Get() {
        static EntityOutline s_instance;
        return s_instance;
    }

    bool EntityOutline::Initialize() {
        if (m_ready) return true;
        if (m_initTried || !g_renderBackend) return false;
        m_initTried = true;

        RenderBackend& b = *g_renderBackend;
        // Plain (texture + push constant) shaders on Vulkan: none of these
        // reads the Common UBO. (Their samplers are not named uTex: the
        // shader-pack pipeline picks gbuffers_block programs by that name.)
        m_silhouetteShader = b.CreateShaderFromFiles("shaders/entity_outline.vert", "shaders/entity_outline.frag");
        m_sobelShader = b.CreateShaderFromFiles("shaders/entity_outline_post.vert", "shaders/entity_outline_sobel.frag");
        m_blurShader  = b.CreateShaderFromFiles("shaders/entity_outline_post.vert", "shaders/entity_outline_blur.frag");
        m_blitShader  = b.CreateShaderFromFiles("shaders/entity_outline_post.vert", "shaders/entity_outline_blit.frag");
        if (m_silhouetteShader == INVALID_SHADER || m_sobelShader == INVALID_SHADER ||
            m_blurShader == INVALID_SHADER || m_blitShader == INVALID_SHADER) {
            Log::Warning("[EntityOutline] shaders failed to load — glowing entities will not be outlined");
            ReleaseResources();
            return false;
        }

        // Two triangles over the screen. u/v follow GL (0,0 at the bottom
        // left); the Vulkan vertex shader mirrors v for its flipped viewport.
        const QuadVert quad[6] = {
            {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
            { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255},
            { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
            {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
            { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
            {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
        };
        m_quadVB = b.CreateBuffer(BufferUsage::Vertex, sizeof(quad), quad, BufferAccess::Static);
        if (m_quadVB != INVALID_BUFFER) {
            m_quadMesh = b.CreateMesh(m_quadVB, INVALID_BUFFER, GetBlockVertexLayout());
        }
        if (m_quadMesh == INVALID_MESH) {
            Log::Warning("[EntityOutline] quad mesh failed — glowing entities will not be outlined");
            ReleaseResources();
            return false;
        }
        m_ready = true;
        return true;
    }

    void EntityOutline::Shutdown() {
        ReleaseResources();
        m_initTried = false;   // a later session initialises afresh
    }

    void EntityOutline::ReleaseResources() {
        m_ready = false;
        m_requests.clear();
        if (!g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;
        for (ShaderHandle* s : {&m_silhouetteShader, &m_sobelShader, &m_blurShader, &m_blitShader}) {
            if (*s != INVALID_SHADER) { b.DestroyShader(*s); *s = INVALID_SHADER; }
        }
        for (RenderTargetHandle* t : {&m_outlineTarget, &m_swapTarget}) {
            if (*t != INVALID_RENDER_TARGET) { b.DestroyRenderTarget(*t); *t = INVALID_RENDER_TARGET; }
        }
        if (m_quadMesh != INVALID_MESH) { b.DestroyMesh(m_quadMesh); m_quadMesh = INVALID_MESH; }
        if (m_quadVB != INVALID_BUFFER) { b.DestroyBuffer(m_quadVB); m_quadVB = INVALID_BUFFER; }
        m_targetWidth = m_targetHeight = 0;
    }

    void EntityOutline::BeginFrame() {
        m_requests.clear();
        m_collecting = false;
        if (!m_ready && !m_initTried) Initialize();
    }

    void EntityOutline::SubmitIndexed(MeshHandle mesh, uint32_t firstIndex, uint32_t indexCount,
                                      TextureHandle texture, const glm::mat4& mvp, uint32_t teamColor) {
        if (!Collecting() || mesh == INVALID_MESH || indexCount == 0 || texture == INVALID_TEXTURE) return;
        m_requests.push_back({mesh, firstIndex, indexCount, true, texture, mvp, TeamColorRGB(teamColor)});
    }

    void EntityOutline::SubmitArrays(MeshHandle mesh, uint32_t firstVertex, uint32_t vertexCount,
                                     TextureHandle texture, const glm::mat4& mvp, uint32_t teamColor) {
        if (!Collecting() || mesh == INVALID_MESH || vertexCount == 0 || texture == INVALID_TEXTURE) return;
        m_requests.push_back({mesh, firstVertex, vertexCount, false, texture, mvp, TeamColorRGB(teamColor)});
    }

    bool EntityOutline::EnsureTargets(int width, int height) {
        RenderBackend& b = *g_renderBackend;
        if (m_outlineTarget == INVALID_RENDER_TARGET || m_swapTarget == INVALID_RENDER_TARGET) {
            RenderTargetDesc desc;
            desc.width = width;
            desc.height = height;
            // MC's TextureTarget("Entity Outline", ..., RGBA8_UNORM).
            desc.colorFormat = TextureFormat::RGBA8;
            if (m_outlineTarget == INVALID_RENDER_TARGET) m_outlineTarget = b.CreateRenderTarget(desc);
            if (m_swapTarget == INVALID_RENDER_TARGET)    m_swapTarget = b.CreateRenderTarget(desc);
            if (m_outlineTarget == INVALID_RENDER_TARGET || m_swapTarget == INVALID_RENDER_TARGET) {
                Log::Warning("[EntityOutline] render targets unavailable — glowing entities will not be outlined");
                ReleaseResources();
                return false;
            }
            m_targetWidth = width;
            m_targetHeight = height;
            // The post chain samples between texels (the blur is bilinear
            // by MC's chain), and never past the edge.
            for (RenderTargetHandle t : {m_outlineTarget, m_swapTarget}) {
                const TextureHandle tex = b.GetRenderTargetColorTexture(t);
                b.SetTextureFilter(tex, TextureFilter::Linear, TextureFilter::Linear);
                b.SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
        }
        if (m_targetWidth != width || m_targetHeight != height) {
            b.ResizeRenderTarget(m_outlineTarget, width, height);
            b.ResizeRenderTarget(m_swapTarget, width, height);
            m_targetWidth = width;
            m_targetHeight = height;
            for (RenderTargetHandle t : {m_outlineTarget, m_swapTarget}) {
                const TextureHandle tex = b.GetRenderTargetColorTexture(t);
                b.SetTextureFilter(tex, TextureFilter::Linear, TextureFilter::Linear);
                b.SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
        }
        return true;
    }

    void EntityOutline::PostPass(ShaderHandle shader, RenderTargetHandle input, RenderTargetHandle output,
                                 const glm::vec2& blurDir) {
        RenderBackend& b = *g_renderBackend;
        b.BindRenderTarget(output);
        b.SetViewport(0, 0, m_targetWidth, m_targetHeight);
        b.BindShader(shader);
        b.BindTexture(b.GetRenderTargetColorTexture(input), 0);
        b.SetUniformInt(shader, "uInSampler", 0);
        b.SetUniformVec2(shader, "uInSize", glm::vec2(static_cast<float>(m_targetWidth),
                                                      static_cast<float>(m_targetHeight)));
        b.SetUniformVec2(shader, "uBlurDir", blurDir);
        b.DrawArrays(m_quadMesh, 6, 0);
        b.UnbindMesh();
    }

    void EntityOutline::Composite(int framebufferWidth, int framebufferHeight) {
        if (m_requests.empty() || !m_ready || !g_renderBackend) {
            m_requests.clear();
            return;
        }
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            m_requests.clear();
            return;
        }
        PROFILE_ZONE_N("EntityOutline");
        if (!EnsureTargets(framebufferWidth, framebufferHeight)) return;
        RenderBackend& b = *g_renderBackend;

        // No depth anywhere in the chain (MC's outline pipelines have no
        // depth-stencil state, its target no depth attachment); no culling
        // (a silhouette is the same from either side).
        PipelineState flat;
        flat.depthTestEnabled  = false;
        flat.depthWriteEnabled = false;
        flat.blendEnabled      = false;
        flat.cullMode          = CullMode::None;
        flat.primitiveType     = PrimitiveType::Triangles;

        // ── 1. The silhouettes (MC executeOutline) ─────────────────────────
        b.BindRenderTarget(m_outlineTarget);
        b.SetViewport(0, 0, m_targetWidth, m_targetHeight);
        // The state first: GL's clear honours the colour write mask the
        // last draw left (a portal pass may have turned it off).
        b.SetPipelineState(flat);
        b.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // MC ZERO_CLEAR_COLOR
        b.Clear(true, false, false);
        b.BindShader(m_silhouetteShader);
        b.SetUniformInt(m_silhouetteShader, "uSilhouetteTex", 0);
        for (const Request& r : m_requests) {
            b.BindTexture(r.texture, 0);
            b.SetUniformMat4(m_silhouetteShader, "uMVP", r.mvp);
            b.SetUniformVec4(m_silhouetteShader, "uColor", glm::vec4(r.color, 1.0f));
            if (r.indexed) b.DrawIndexed(r.mesh, r.count, r.first);
            else           b.DrawArrays(r.mesh, r.count, r.first);
        }
        b.UnbindMesh();
        m_requests.clear();

        // ── 2. post_effect/entity_outline.json ─────────────────────────────
        PostPass(m_sobelShader, m_outlineTarget, m_swapTarget, glm::vec2(0.0f));
        PostPass(m_blurShader,  m_swapTarget, m_outlineTarget, glm::vec2(1.0f, 0.0f));
        PostPass(m_blurShader,  m_outlineTarget, m_swapTarget, glm::vec2(0.0f, 1.0f));

        // ── 3. blitEntityOutline: over the frame, blended, depth ignored ──
        b.BindRenderTarget(INVALID_RENDER_TARGET);
        b.SetViewport(0, 0, framebufferWidth, framebufferHeight);
        PipelineState blend = flat;
        blend.blendEnabled   = true;
        blend.srcBlendFactor = BlendFactor::SrcAlpha;
        blend.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
        b.SetPipelineState(blend);
        b.BindShader(m_blitShader);
        b.BindTexture(b.GetRenderTargetColorTexture(m_swapTarget), 0);
        b.SetUniformInt(m_blitShader, "uInSampler", 0);
        b.DrawArrays(m_quadMesh, 6, 0);
        b.UnbindMesh();

        // Leave the frame's own clear colour (the fog) and the default
        // pipeline as the frame had them.
        const glm::vec3 fog = EnvironmentState::Get().Frame().fogColor;
        b.SetClearColor(fog.r, fog.g, fog.b, 1.0f);
        PipelineState def;
        b.SetPipelineState(def);
    }

} // namespace Render
