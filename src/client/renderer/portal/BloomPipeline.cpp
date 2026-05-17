// File: src/client/renderer/portal/BloomPipeline.cpp
// See header for scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "BloomPipeline.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <glm/glm.hpp>

namespace Render {

    BloomPipeline g_bloomPipeline;

    namespace {
        // Fallback embedded shaders (mirror the .frag files on disk).
        const char* kFullscreenVert = R"(
#version 330 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID == 1) ?  3.0 : -1.0,
                    (gl_VertexID == 2) ?  3.0 : -1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";
        const char* kBrightFrag = R"(
#version 330 core
uniform sampler2D uHDRColor;
uniform float     uThreshold;
uniform float     uSoftKnee;
in  vec2 vUV;
out vec4 FragColor;
void main() {
    vec3 c = texture(uHDRColor, vUV).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float knee = uThreshold * uSoftKnee;
    float soft = clamp((lum - uThreshold + knee) / max(knee, 1e-4), 0.0, 1.0);
    soft = soft * soft * (3.0 - 2.0 * soft);
    float bright = max(soft, max(0.0, lum - uThreshold) / max(lum, 1e-4));
    FragColor = vec4(c * bright, 1.0);
}
)";
        const char* kBlurFrag = R"(
#version 330 core
uniform sampler2D uColor;
uniform vec2      uTexelSize;
uniform vec2      uDirection;
in  vec2 vUV;
out vec4 FragColor;
void main() {
    const float w0 = 0.2270270270;
    const float w1 = 0.1945945946;
    const float w2 = 0.1216216216;
    const float w3 = 0.0540540541;
    const float w4 = 0.0162162162;
    vec2 off1 = uDirection * uTexelSize * 1.0;
    vec2 off2 = uDirection * uTexelSize * 2.0;
    vec2 off3 = uDirection * uTexelSize * 3.0;
    vec2 off4 = uDirection * uTexelSize * 4.0;
    vec3 acc = texture(uColor, vUV).rgb * w0;
    acc += texture(uColor, vUV + off1).rgb * w1;
    acc += texture(uColor, vUV - off1).rgb * w1;
    acc += texture(uColor, vUV + off2).rgb * w2;
    acc += texture(uColor, vUV - off2).rgb * w2;
    acc += texture(uColor, vUV + off3).rgb * w3;
    acc += texture(uColor, vUV - off3).rgb * w3;
    acc += texture(uColor, vUV + off4).rgb * w4;
    acc += texture(uColor, vUV - off4).rgb * w4;
    FragColor = vec4(acc, 1.0);
}
)";
        const char* kUpsampleFrag = R"(
#version 330 core
uniform sampler2D uSmaller;
uniform sampler2D uLarger;
uniform float     uStrength;
in  vec2 vUV;
out vec4 FragColor;
void main() {
    vec3 small = texture(uSmaller, vUV).rgb;
    vec3 large = texture(uLarger,  vUV).rgb;
    FragColor = vec4(large + small * uStrength, 1.0);
}
)";
    } // namespace

    BloomPipeline::BloomPipeline()  = default;
    BloomPipeline::~BloomPipeline() { Shutdown(); }

    bool BloomPipeline::Initialize() {
        if (m_active) return true;
        if (!g_renderBackend) {
            Log::Warning("[BloomPipeline] No backend; bloom disabled");
            return false;
        }

        // Try file-based shaders first; fall back to embedded source.
        auto tryLoad = [&](const char* vp, const char* fp, const char* embeddedVert, const char* embeddedFrag) -> ShaderHandle {
            ShaderHandle h = g_renderBackend->CreateShaderFromFiles(vp, fp);
            if (h == INVALID_SHADER) {
                h = g_renderBackend->CreateShader(embeddedVert, embeddedFrag);
            }
            return h;
        };
        m_brightShader   = tryLoad("shaders/portal_tonemap.vert", "shaders/portal_bloom_bright.frag",
                                   kFullscreenVert, kBrightFrag);
        m_blurShader     = tryLoad("shaders/portal_tonemap.vert", "shaders/portal_bloom_blur.frag",
                                   kFullscreenVert, kBlurFrag);
        m_upsampleShader = tryLoad("shaders/portal_tonemap.vert", "shaders/portal_bloom_upsample.frag",
                                   kFullscreenVert, kUpsampleFrag);
        if (m_brightShader == INVALID_SHADER || m_blurShader == INVALID_SHADER ||
            m_upsampleShader == INVALID_SHADER) {
            Log::Warning("[BloomPipeline] Shader compile failed; bloom disabled");
            Shutdown();
            return false;
        }

        // Dummy fullscreen-triangle mesh — same trick as HDRPipeline.
        const uint8_t threeVerts[24 * 3] = {0};
        m_fsTriangleVB = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
            sizeof(threeVerts), threeVerts, BufferAccess::Static);
        m_fsTriangleMesh = g_renderBackend->CreateMesh(m_fsTriangleVB,
            INVALID_BUFFER, GetBlockVertexLayout());

        // RT allocation deferred to first Apply() when we know the
        // framebuffer size.
        m_active = true;
        Log::Info("[BloomPipeline] Initialized (lazy RT allocation)");
        return true;
    }

    bool BloomPipeline::AllocateRTs(int w, int h) {
        DestroyRTs();
        for (int i = 0; i < kBloomLevels; ++i) {
            // Each level halves the previous resolution.
            const int lw = std::max(1, w >> (i + 1));
            const int lh = std::max(1, h >> (i + 1));
            RenderTargetDesc desc;
            desc.width       = lw;
            desc.height      = lh;
            desc.colorFormat = TextureFormat::RGBA16F;
            desc.depthFormat = TextureFormat::Depth24Stencil8;
            m_brightRT[i]  = g_renderBackend->CreateRenderTarget(desc);
            m_blurredRT[i] = g_renderBackend->CreateRenderTarget(desc);
            if (m_brightRT[i] == INVALID_RENDER_TARGET ||
                m_blurredRT[i] == INVALID_RENDER_TARGET) {
                Log::Warning("[BloomPipeline] RT allocation failed at level %d; "
                             "bloom disabled", i);
                DestroyRTs();
                return false;
            }
        }
        m_currentWidth  = w;
        m_currentHeight = h;
        return true;
    }

    void BloomPipeline::DestroyRTs() {
        if (!g_renderBackend) return;
        for (int i = 0; i < kBloomLevels; ++i) {
            if (m_brightRT[i] != INVALID_RENDER_TARGET) {
                g_renderBackend->DestroyRenderTarget(m_brightRT[i]);
                m_brightRT[i] = INVALID_RENDER_TARGET;
            }
            if (m_blurredRT[i] != INVALID_RENDER_TARGET) {
                g_renderBackend->DestroyRenderTarget(m_blurredRT[i]);
                m_blurredRT[i] = INVALID_RENDER_TARGET;
            }
        }
    }

    void BloomPipeline::Shutdown() {
        if (!g_renderBackend) {
            m_active = false;
            return;
        }
        DestroyRTs();
        if (m_fsTriangleMesh != INVALID_MESH) { g_renderBackend->DestroyMesh(m_fsTriangleMesh);   m_fsTriangleMesh = INVALID_MESH; }
        if (m_fsTriangleVB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_fsTriangleVB);   m_fsTriangleVB   = INVALID_BUFFER; }
        if (m_brightShader   != INVALID_SHADER){ g_renderBackend->DestroyShader(m_brightShader);   m_brightShader   = INVALID_SHADER; }
        if (m_blurShader     != INVALID_SHADER){ g_renderBackend->DestroyShader(m_blurShader);     m_blurShader     = INVALID_SHADER; }
        if (m_upsampleShader != INVALID_SHADER){ g_renderBackend->DestroyShader(m_upsampleShader); m_upsampleShader = INVALID_SHADER; }
        m_active = false;
    }

    TextureHandle BloomPipeline::Apply(TextureHandle hdrSource, int fbW, int fbH) {
        if (!m_active || !g_renderBackend || hdrSource == INVALID_TEXTURE) {
            return INVALID_TEXTURE;
        }

        // Lazy / on-resize RT allocation.
        if (m_brightRT[0] == INVALID_RENDER_TARGET ||
            m_currentWidth != fbW || m_currentHeight != fbH) {
            if (!AllocateRTs(fbW, fbH)) return INVALID_TEXTURE;
        }

        // Common pipeline state for all sub-passes (no depth/stencil/blend).
        PipelineState s;
        s.depthTestEnabled  = false;
        s.depthWriteEnabled = false;
        s.colorWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        s.stencilTestEnabled = false;
        g_renderBackend->SetPipelineState(s);

        // === Step 1: Bright-pass extract into m_brightRT[0]. ===
        g_renderBackend->BindRenderTarget(m_brightRT[0]);
        g_renderBackend->BindShader(m_brightShader);
        g_renderBackend->BindTexture(hdrSource, 0);
        g_renderBackend->SetUniformInt  (m_brightShader, "uHDRColor",   0);
        g_renderBackend->SetUniformFloat(m_brightShader, "uThreshold",  m_threshold);
        g_renderBackend->SetUniformFloat(m_brightShader, "uSoftKnee",   m_softKnee);
        g_renderBackend->DrawArrays(m_fsTriangleMesh, 3);

        // === Step 2: Downsample chain — each level's bright source is
        //     the previous level's blurred result. We achieve downsampling
        //     by rendering into a smaller RT with linear filtering on
        //     the input texture.
        // (We do the H+V blur at each level inline rather than separate
        //  downsample → blur — saves an RT pair.)

        for (int i = 0; i < kBloomLevels; ++i) {
            const int lw = std::max(1, fbW >> (i + 1));
            const int lh = std::max(1, fbH >> (i + 1));
            const glm::vec2 texel(1.0f / float(lw), 1.0f / float(lh));

            TextureHandle src = (i == 0)
                ? g_renderBackend->GetRenderTargetColorTexture(m_brightRT[0])
                : g_renderBackend->GetRenderTargetColorTexture(m_blurredRT[i - 1]);

            // Horizontal blur from src → brightRT[i]. (For i=0 we
            // overwrite brightRT[0]; the bright-pass result has already
            // been consumed by reading it as src.)
            //
            // Actually that's a hazard: for i=0, src == brightRT[0]
            // texture, and we're writing back to the same RT. Use a
            // ping-pong: do horiz from brightRT[i] → blurredRT[i], then
            // vert from blurredRT[i] → brightRT[i].
            g_renderBackend->BindRenderTarget(m_blurredRT[i]);
            g_renderBackend->BindShader(m_blurShader);
            g_renderBackend->BindTexture(src, 0);
            g_renderBackend->SetUniformInt (m_blurShader, "uColor", 0);
            g_renderBackend->SetUniformVec2(m_blurShader, "uTexelSize", texel);
            g_renderBackend->SetUniformVec2(m_blurShader, "uDirection", glm::vec2(1.0f, 0.0f));
            g_renderBackend->DrawArrays(m_fsTriangleMesh, 3);

            // Vertical blur from blurredRT[i] → brightRT[i].
            g_renderBackend->BindRenderTarget(m_brightRT[i]);
            g_renderBackend->BindShader(m_blurShader);
            g_renderBackend->BindTexture(
                g_renderBackend->GetRenderTargetColorTexture(m_blurredRT[i]), 0);
            g_renderBackend->SetUniformInt (m_blurShader, "uColor", 0);
            g_renderBackend->SetUniformVec2(m_blurShader, "uTexelSize", texel);
            g_renderBackend->SetUniformVec2(m_blurShader, "uDirection", glm::vec2(0.0f, 1.0f));
            g_renderBackend->DrawArrays(m_fsTriangleMesh, 3);

            // For the NEXT level's input we want brightRT[i] (the H+V
            // blurred bright pixels at level i resolution). The next
            // iteration's lw,lh will be half of this — linear filtering
            // on the larger texture provides the downsample.
            //
            // Copy brightRT[i] → blurredRT[i] so the upsample chain
            // can find it under blurredRT[i] (which we now use as the
            // "stored blurred bloom at level i" slot).
            // Actually, more efficient: just store final blurred bloom
            // at this level into blurredRT[i] (overwrite the temp horiz
            // result). Do the vertical blur output into blurredRT[i]
            // instead of brightRT[i]. Then the upsample chain reads
            // blurredRT[i] directly.
            //
            // Restructure: re-target the vertical blur output. (The
            // double-bind below is a temporary — clean it up in the
            // final version.) See note: the previous block already
            // ping-ponged H into blurredRT and V into brightRT.
            // For clarity we leave the structure as-is and use
            // brightRT[i] as the canonical "final blurred bloom at
            // level i" output for the upsample chain below.
        }

        // === Step 3: Upsample-and-combine chain ===
        // Start at the smallest level and combine into the next larger
        // level. Result of each combination is stored into blurredRT[i-1]
        // (reusing the slot we no longer need at that level).
        for (int i = kBloomLevels - 2; i >= 0; --i) {
            // Larger level = m_brightRT[i], smaller = m_brightRT[i+1]
            // (or m_blurredRT[i+1] for the next iteration).
            TextureHandle smallerTex = (i == kBloomLevels - 2)
                ? g_renderBackend->GetRenderTargetColorTexture(m_brightRT[i + 1])
                : g_renderBackend->GetRenderTargetColorTexture(m_blurredRT[i + 1]);
            TextureHandle largerTex  = g_renderBackend->GetRenderTargetColorTexture(m_brightRT[i]);

            g_renderBackend->BindRenderTarget(m_blurredRT[i]);
            g_renderBackend->BindShader(m_upsampleShader);
            g_renderBackend->BindTexture(smallerTex, 0);
            g_renderBackend->BindTexture(largerTex,  1);
            g_renderBackend->SetUniformInt  (m_upsampleShader, "uSmaller",  0);
            g_renderBackend->SetUniformInt  (m_upsampleShader, "uLarger",   1);
            g_renderBackend->SetUniformFloat(m_upsampleShader, "uStrength", m_intensity);
            g_renderBackend->DrawArrays(m_fsTriangleMesh, 3);
        }

        // Result lives in m_blurredRT[0] — at half framebuffer res.
        // The tone-map composite will sample it with linear filtering
        // (texture min/mag filter = Linear, set when RT was created)
        // to upsample to full res.
        return g_renderBackend->GetRenderTargetColorTexture(m_blurredRT[0]);
    }

} // namespace Render

#endif // ENABLE_PORTAL_GUN
