// File: src/client/renderer/portal/HDRPipeline.cpp
// See header for scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "HDRPipeline.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"

#include <GLFW/glfw3.h>

namespace Render {

    HDRPipeline g_hdrPipeline;

    HDRPipeline::HDRPipeline()  = default;
    HDRPipeline::~HDRPipeline() { Shutdown(); }

    namespace {
        // Embedded fallback shaders if shaders/portal_tonemap.* aren't on
        // disk (e.g., release build with assets bundled differently).
        const char* kToneMapVert = R"(
#version 330 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID == 1) ?  3.0 : -1.0,
                    (gl_VertexID == 2) ?  3.0 : -1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";
        const char* kToneMapFrag = R"(
#version 330 core
uniform sampler2D uHDRColor;
uniform sampler2D uBloomColor;
uniform float     uExposure;
uniform float     uHasBloom;
in  vec2 vUV;
out vec4 FragColor;
// Pass-through composite: matches the pre-HDR-pipeline LDR render
// path so the sky / scene colours look identical to before. ACES
// removed because it desaturates normal LDR pixels and made the sky
// read as grey.
void main() {
    vec3 hdr = texture(uHDRColor, vUV).rgb;
    if (uHasBloom > 0.5) hdr += texture(uBloomColor, vUV).rgb;
    hdr *= uExposure;
    FragColor = vec4(clamp(hdr, 0.0, 1.0), 1.0);
}
)";
    } // namespace

    bool HDRPipeline::Initialize() {
        if (m_active) return true;
        if (!g_renderBackend) {
            Log::Warning("[HDRPipeline] No backend; HDR disabled");
            return false;
        }

        // Probe RT support — get framebuffer size for the initial RT.
        int fbW = 1280, fbH = 720;
        if (g_renderBackend->GetWindow()) {
            glfwGetFramebufferSize(g_renderBackend->GetWindow(), &fbW, &fbH);
        }
        if (fbW <= 0) fbW = 1280;
        if (fbH <= 0) fbH = 720;

        RenderTargetDesc desc;
        desc.width       = fbW;
        desc.height      = fbH;
        desc.colorFormat = TextureFormat::RGBA16F;
        desc.depthFormat = TextureFormat::Depth24Stencil8;
        m_hdrRT = g_renderBackend->CreateRenderTarget(desc);
        if (m_hdrRT == INVALID_RENDER_TARGET) {
            Log::Warning("[HDRPipeline] Backend lacks RT support; HDR disabled "
                         "(rendering will go directly to LDR backbuffer)");
            return false;
        }

        // Tone map shader. Try file first, fall back to embedded source.
        m_toneMapShader = g_renderBackend->CreateShaderFromFiles(
            "shaders/portal_tonemap.vert", "shaders/portal_tonemap.frag");
        if (m_toneMapShader == INVALID_SHADER) {
            m_toneMapShader = g_renderBackend->CreateShader(
                kToneMapVert, kToneMapFrag);
        }
        if (m_toneMapShader == INVALID_SHADER) {
            Log::Warning("[HDRPipeline] Tone-map shader compile failed; HDR disabled");
            g_renderBackend->DestroyRenderTarget(m_hdrRT);
            m_hdrRT = INVALID_RENDER_TARGET;
            return false;
        }

        // Dummy 1x1 black texture for the "no bloom" sampler binding —
        // the tone-map shader always samples uBloomColor even when
        // uHasBloom = 0 (avoids unbound-sampler validation errors).
        unsigned char black[] = {0, 0, 0, 0};
        m_dummyBloom = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, black);

        // Dummy mesh for fullscreen triangle — the vertex shader
        // generates positions from gl_VertexID so the VBO data is
        // unused. Allocate 3 verts worth of zeros (72 bytes for the
        // 24-byte block vertex layout) so the driver has valid memory
        // to read from when fetching vertex attributes (it does the
        // fetch even though the shader doesn't use them).
        const uint8_t threeVerts[24 * 3] = {0};
        m_fsTriangleVB = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
            sizeof(threeVerts), threeVerts, BufferAccess::Static);
        m_fsTriangleMesh = g_renderBackend->CreateMesh(m_fsTriangleVB,
            INVALID_BUFFER, GetBlockVertexLayout());

        m_currentWidth  = fbW;
        m_currentHeight = fbH;
        m_active        = true;

        Log::Info("[HDRPipeline] Initialized (%dx%d, RGBA16F)", fbW, fbH);
        return true;
    }

    void HDRPipeline::Shutdown() {
        if (!g_renderBackend) {
            m_active = false;
            return;
        }
        if (m_fsTriangleMesh != INVALID_MESH) { g_renderBackend->DestroyMesh(m_fsTriangleMesh);          m_fsTriangleMesh = INVALID_MESH; }
        if (m_fsTriangleVB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(m_fsTriangleVB);          m_fsTriangleVB   = INVALID_BUFFER; }
        if (m_dummyBloom     != INVALID_TEXTURE){g_renderBackend->DestroyTexture(m_dummyBloom);           m_dummyBloom     = INVALID_TEXTURE; }
        if (m_toneMapShader  != INVALID_SHADER){ g_renderBackend->DestroyShader(m_toneMapShader);         m_toneMapShader  = INVALID_SHADER; }
        if (m_hdrRT          != INVALID_RENDER_TARGET){ g_renderBackend->DestroyRenderTarget(m_hdrRT);    m_hdrRT          = INVALID_RENDER_TARGET; }
        m_active = false;
    }

    TextureHandle HDRPipeline::GetHDRColorTexture() const {
        if (!m_active || !g_renderBackend) return INVALID_TEXTURE;
        return g_renderBackend->GetRenderTargetColorTexture(m_hdrRT);
    }

    void HDRPipeline::BeginHDRPass(int w, int h) {
        if (!m_active || !g_renderBackend) return;
        if (w != m_currentWidth || h != m_currentHeight) {
            g_renderBackend->ResizeRenderTarget(m_hdrRT, w, h);
            m_currentWidth  = w;
            m_currentHeight = h;
        }
        g_renderBackend->BindRenderTarget(m_hdrRT);
        // Clear the HDR target — color + depth + stencil. Use a slightly
        // brighter sky-ish blue clear so the rendered scene blends with
        // the existing LDR clear color when the pipeline is disabled.
        g_renderBackend->SetClearColor(120.0f / 255.0f,
                                       167.0f / 255.0f,
                                       255.0f / 255.0f, 1.0f);
        g_renderBackend->Clear(true, true, true);
    }

    void HDRPipeline::EndHDRPassAndComposite(TextureHandle bloomColor) {
        if (!m_active || !g_renderBackend) return;

        // Switch to backbuffer.
        g_renderBackend->BindRenderTarget(INVALID_RENDER_TARGET);
        // Reset viewport to current framebuffer size — BindRenderTarget(INVALID)
        // doesn't change viewport, and the previous Bind set it to RT size.
        g_renderBackend->SetViewport(0, 0, m_currentWidth, m_currentHeight);

        // No depth/stencil/blend; just write color from the tone-mapped HDR.
        PipelineState s;
        s.depthTestEnabled  = false;
        s.depthWriteEnabled = false;
        s.colorWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        s.stencilTestEnabled = false;
        g_renderBackend->SetPipelineState(s);

        g_renderBackend->BindShader(m_toneMapShader);

        // Bind HDR color to slot 0.
        const TextureHandle hdrTex =
            g_renderBackend->GetRenderTargetColorTexture(m_hdrRT);
        g_renderBackend->BindTexture(hdrTex, 0);
        g_renderBackend->SetUniformInt(m_toneMapShader, "uHDRColor", 0);

        // Bind bloom (or dummy) to slot 1.
        const bool hasBloom = (bloomColor != INVALID_TEXTURE);
        g_renderBackend->BindTexture(hasBloom ? bloomColor : m_dummyBloom, 1);
        g_renderBackend->SetUniformInt(m_toneMapShader, "uBloomColor", 1);
        g_renderBackend->SetUniformFloat(m_toneMapShader, "uHasBloom",
                                         hasBloom ? 1.0f : 0.0f);
        g_renderBackend->SetUniformFloat(m_toneMapShader, "uExposure", m_exposure);

        // Draw 3-vert fullscreen triangle. Vertex shader synthesises
        // positions from gl_VertexID — VBO is unused but a mesh must
        // be bound for the backend to issue a draw call.
        g_renderBackend->DrawArrays(m_fsTriangleMesh, 3);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render

#endif // ENABLE_PORTAL_GUN
