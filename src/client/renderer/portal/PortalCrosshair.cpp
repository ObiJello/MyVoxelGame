// File: src/client/renderer/portal/PortalCrosshair.cpp
// See header for scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalCrosshair.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include "../../../ext/stb_image/stb_image.h"

#include <GLFW/glfw3.h>   // for glfwGetTime() — same clock the rest of the engine uses
#include <algorithm>
#include <cmath>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    PortalCrosshair g_portalCrosshair;

    namespace {
        // Atlas regions copied verbatim from
        // Portal-Root/scripts/mod_textures.txt under "PortalSprites".
        // Atlas total is 256×64 (portal_crosshairs.vtf).
        struct AtlasRegion { int x, y, w, h; };
        constexpr AtlasRegion kLeftInvalid   {  2, 0, 44, 64 };
        constexpr AtlasRegion kRightInvalid  { 50, 0, 44, 64 };
        constexpr AtlasRegion kLeftValid     { 98, 0, 44, 64 };
        constexpr AtlasRegion kRightValid    {146, 0, 44, 64 };
        constexpr AtlasRegion kLastPlaced    {194, 0, 28, 64 };

        // Bracket alpha — matches Portal's iAlphaStart=150 baseline so
        // the outline reads as visibly present without competing with
        // the world. The "filled" valid sprite uses the same alpha so
        // the swap is purely a shape change, not a brightness change.
        constexpr uint8_t kBracketAlpha = 220;

        // Portal's UTIL_Portal_Color (1, 2): Source's stock values.
        // Slight saturation tweak: portal1=blue, portal2=orange.
        constexpr glm::vec3 kBlueRGB  { 64.0f / 255.0f, 160.0f / 255.0f, 255.0f / 255.0f };
        constexpr glm::vec3 kOrangeRGB{255.0f / 255.0f, 160.0f / 255.0f,  32.0f / 255.0f };

        // Inline GLSL — matches the existing Crosshair shader's layout
        // (pos3 + uv2 + color4 ubyte) so we can reuse GetBlockVertexLayout().
        constexpr const char* kVertSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
uniform vec2 uUVMin;
uniform vec2 uUVMax;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    // Map the unit-quad UV [0,1] into the atlas region rectangle.
    vUV = mix(uUVMin, uUVMax, aUV);
}
)GLSL";
        constexpr const char* kFragSrc = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uAtlas;
uniform vec4      uTint;     // tint colour (rgb) + alpha multiplier (a)
out vec4 FragColor;
void main() {
    vec4 t = texture(uAtlas, vUV);
    // Atlas is bracket art with an alpha mask. Tint multiplies the
    // colour and alpha — equivalent to Source's DrawSelf(color).
    FragColor = vec4(uTint.rgb * t.rgb, uTint.a * t.a);
}
)GLSL";

        // Vertex layout for a unit quad — `aPos` is xy in [0,1], `aUV`
        // is the same (we'll remap to atlas region in the vertex shader),
        // `aColor` is the per-vertex tint.
        struct QuadVertex {
            float    x, y;
            float    z;       // padding so we match the stride of GetBlockVertexLayout (3 floats pos)
            float    u, v;
            uint8_t  r, g, b, a;
        };
        static_assert(sizeof(QuadVertex) == 24, "must match block vertex layout stride");

        // Per-side placement state. False = no portal of that colour
        // is currently on a wall → draw the outline (invalid sprite,
        // tinted in colour). True = portal active → draw the filled
        // (valid sprite) variant. Flips via Notify* below.
        bool s_blueActive   = false;
        bool s_orangeActive = false;
    } // namespace

    void PortalCrosshair::NotifyPortalPlaced(uint8_t color) {
        if (color == 0)      s_blueActive   = true;
        else if (color == 1) s_orangeActive = true;
    }

    void PortalCrosshair::NotifyPortalRemoved(uint8_t color) {
        // PortalRemoveS2C uses color=2 to mean "clear both".
        if (color == 0 || color == 2) s_blueActive   = false;
        if (color == 1 || color == 2) s_orangeActive = false;
    }

    bool PortalCrosshair::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) {
            Log::Warning("[PortalCrosshair] No render backend; HUD disabled");
            return false;
        }

        // Shader — try files first so Vulkan picks up the pre-compiled
        // _vk.spv (CreateShaderFromFiles on the GL backend reads the
        // .vert/.frag GLSL text; on Vulkan it maps to _vk.*.spv via
        // the path-rewrite in VKBackend::CreateShaderFromFiles). The
        // inline source is a fallback for the GL backend if the files
        // aren't on disk for some reason.
        const std::string vertPath = PlatformMain::GetAssetPath("shaders/portal_crosshair.vert");
        const std::string fragPath = PlatformMain::GetAssetPath("shaders/portal_crosshair.frag");
        m_shader = g_renderBackend->CreateShaderFromFiles(vertPath, fragPath);
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShader(kVertSrc, kFragSrc);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[PortalCrosshair] Shader compile failed");
            return false;
        }

        // Atlas
        const std::string atlasPath = PlatformMain::GetAssetPath(
            "assets/textures/gui/portal/portal_crosshairs.png");
        int channels = 0;
        // Don't flip — we'll author UVs in the convention the PNG sits on disk.
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(atlasPath.c_str(), &m_atlasW, &m_atlasH, &channels, 4);
        if (!pixels) {
            Log::Warning("[PortalCrosshair] Failed to load atlas %s (%s)",
                         atlasPath.c_str(), stbi_failure_reason());
            return false;
        }
        m_atlas = g_renderBackend->CreateTexture2D(m_atlasW, m_atlasH, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);
        if (m_atlas == INVALID_TEXTURE) {
            Log::Warning("[PortalCrosshair] Atlas upload failed");
            return false;
        }
        // Nearest filter — the original art is pixel-pure and bilinear
        // bleeds neighbouring atlas regions across the sprite borders.
        g_renderBackend->SetTextureFilter(m_atlas, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_atlas, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        // Unit quad — positions in [0,1]^2 will be model-scaled per-draw
        // to land at the right size and screen position.
        const QuadVertex verts[4] = {
            {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 255,255,255,255},  // TL
            {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 255,255,255,255},  // TR
            {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 255,255,255,255},  // BR
            {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 255,255,255,255},  // BL
        };
        const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, sizeof(verts), verts);
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index,  sizeof(indices), indices);
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        m_initialized = true;
        Log::Info("[PortalCrosshair] Initialized (atlas %dx%d)", m_atlasW, m_atlasH);
        return true;
    }

    void PortalCrosshair::Shutdown() {
        if (!g_renderBackend) { m_initialized = false; return; }
        if (m_mesh    != INVALID_MESH)    g_renderBackend->DestroyMesh(m_mesh);
        if (m_vb      != INVALID_BUFFER)  g_renderBackend->DestroyBuffer(m_vb);
        if (m_ib      != INVALID_BUFFER)  g_renderBackend->DestroyBuffer(m_ib);
        if (m_atlas   != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_atlas);
        if (m_shader  != INVALID_SHADER)  g_renderBackend->DestroyShader(m_shader);
        m_mesh = INVALID_MESH; m_vb = INVALID_BUFFER; m_ib = INVALID_BUFFER;
        m_atlas = INVALID_TEXTURE; m_shader = INVALID_SHADER;
        m_initialized = false;
    }

    void PortalCrosshair::Render(int windowWidth, int windowHeight,
                                 int framebufferWidth, int framebufferHeight,
                                 float dt) {
        if (!m_initialized || !g_renderBackend) return;
        if (framebufferWidth <= 0 || framebufferHeight <= 0) return;
        if (m_atlasW <= 0 || m_atlasH <= 0) return;

        (void)dt;  // no time-based animation any more

        // Pipeline — standard alpha-blended HUD overlay. Unlike the
        // base crosshair we want translucent overlay, NOT inverted XOR,
        // because the brackets carry their own colour info (blue /
        // orange) that shouldn't get XOR-ed against the scene.
        PipelineState s;
        s.depthTestEnabled  = false;
        s.depthWriteEnabled = false;
        s.blendEnabled      = true;
        s.srcBlendFactor    = BlendFactor::SrcAlpha;
        s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);

        const float fbW = static_cast<float>(framebufferWidth);
        const float fbH = static_cast<float>(framebufferHeight);
        const glm::mat4 ortho = glm::ortho(0.0f, fbW, fbH, 0.0f, -1.0f, 1.0f);

        // Match the existing crosshair's window→framebuffer scale so
        // the brackets stay the same pixel size on Retina displays.
        const float scaleX = static_cast<float>(framebufferWidth) /
                             static_cast<float>(windowWidth);
        // Sprite world-pixel size — twice the size of the base crosshair
        // (32 → 64 px tall on screen) so the brackets read clearly
        // around the centre dot.
        const float bracketH_px = 64.0f * scaleX;

        const float xC = fbW * 0.5f;
        const float yC = fbH * 0.5f;

        auto Draw = [&](const AtlasRegion& region,
                        float screenLeft, float screenTop,
                        float pxScale,
                        glm::vec3 rgb, float alpha01) {
            if (alpha01 <= 0.0f) return;
            const float w = static_cast<float>(region.w) * pxScale;
            const float h = static_cast<float>(region.h) * pxScale;
            glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                             glm::vec3(screenLeft, screenTop, 0.0f));
            model = glm::scale(model, glm::vec3(w, h, 1.0f));
            const glm::mat4 mvp = ortho * model;

            const glm::vec2 uvMin{
                static_cast<float>(region.x) / static_cast<float>(m_atlasW),
                static_cast<float>(region.y) / static_cast<float>(m_atlasH)
            };
            const glm::vec2 uvMax{
                static_cast<float>(region.x + region.w) / static_cast<float>(m_atlasW),
                static_cast<float>(region.y + region.h) / static_cast<float>(m_atlasH)
            };

            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_atlas, 0);
            g_renderBackend->SetUniformInt(m_shader, "uAtlas", 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->SetUniformVec2(m_shader, "uUVMin", uvMin);
            g_renderBackend->SetUniformVec2(m_shader, "uUVMax", uvMax);

            const glm::vec4 tint(rgb.r, rgb.g, rgb.b, alpha01);
            g_renderBackend->SetUniformVec4(m_shader, "uTint", tint);
            g_renderBackend->DrawIndexed(m_mesh, 6);
        };

        // Bracket vertical placement matches hud_quickinfo.cpp:313-315:
        //   yCenter_portal = (ScreenHeight() - icon_lb_portal.Height()) / 2
        // so the bracket pair is vertically centred on the crosshair.
        // The +0.17 * height offset (line 563/568) tilts the left
        // bracket slightly up and the right bracket slightly down.
        const float pxPerAtlasPx = bracketH_px / static_cast<float>(kLeftValid.h);

        const float bracketW = kLeftValid.w * pxPerAtlasPx;
        const float bracketH = kLeftValid.h * pxPerAtlasPx;

        const float yBracket = yC - bracketH * 0.5f;
        // Source offsets reproduced verbatim — keep the two brackets
        // hugging the crosshair the way Portal does (small overlap
        // toward centre, the two ticks reading as a "[ ]" frame).
        const float xLeftBracket  = xC - bracketW * 0.64f;
        const float xRightBracket = xC - bracketW * 0.35f;
        const float yBracketLeft  = yBracket - bracketH * 0.17f;
        const float yBracketRight = yBracket + bracketH * 0.17f;

        // Each side picks one of two atlas regions:
        //   inactive (no portal of that colour on a wall) → "invalid"
        //     sprite, which is the outline-only version. Tinted in
        //     the bracket's own colour so blue reads as a blue outline,
        //     orange as an orange outline.
        //   active (portal placed) → "valid" sprite, the filled-in
        //     bracket. Same tint.
        const AtlasRegion& leftRegion  = s_blueActive   ? kLeftValid  : kLeftInvalid;
        const AtlasRegion& rightRegion = s_orangeActive ? kRightValid : kRightInvalid;

        Draw(leftRegion,  xLeftBracket,  yBracketLeft,  pxPerAtlasPx,
             kBlueRGB,  kBracketAlpha / 255.0f);
        Draw(rightRegion, xRightBracket, yBracketRight, pxPerAtlasPx,
             kOrangeRGB, kBracketAlpha / 255.0f);

        g_renderBackend->UnbindMesh();
    }

} // namespace Render

#endif // ENABLE_PORTAL_GUN
