// File: src/client/renderer/blockentity/EndPortalRenderer.cpp
// See the header for scope and for why this is not a BlockEntityRenderer.

#include "client/resource/ResourcePacks.hpp"
#include "EndPortalRenderer.hpp"

#include "../backend/RenderBackend.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "common/core/Config.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif

#include <stb_image.h>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string&); }

namespace Render {

    EndPortalRenderer g_endPortalRenderer;

    // ── Inline GLSL fallback ─────────────────────────────────────────────
    //
    // CreateShaderFromFiles is tried first so shaders/end_portal.{vert,frag}
    // stay editable without a rebuild; these copies are what a bundled build
    // that cannot find the files falls back to. They must track the files
    // byte for byte — same duplication (and same contract) as
    // BlockBreakOverlay. See the .vert/.frag for the commentary; it is not
    // repeated here.
    const char* EndPortalRenderer::s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;

out vec4 vTexProj;
out vec3 vWorldPos;

vec4 projection_from_position(vec4 position) {
    vec4 projection = position * 0.5;
    projection.xy = vec2(projection.x + projection.w, projection.y + projection.w);
    projection.zw = position.zw;
    return projection;
}

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexProj = projection_from_position(gl_Position);
    vWorldPos = aPos;
}
)";

    const char* EndPortalRenderer::s_fragSource = R"(
#version 330 core
#define PORTAL_LAYERS 15

uniform sampler2D uSky;
uniform sampler2D uPortal;
uniform float uTime;
uniform vec3 uCameraPos;
uniform vec4 uFogColor;
uniform vec4 uFogEnv;

in vec4 vTexProj;
in vec3 vWorldPos;

out vec4 FragColor;

const vec3 COLORS[16] = vec3[16](
    vec3(0.022087, 0.098399, 0.110818),
    vec3(0.011892, 0.095924, 0.089485),
    vec3(0.027636, 0.101689, 0.100326),
    vec3(0.046564, 0.109883, 0.114838),
    vec3(0.064901, 0.117696, 0.097189),
    vec3(0.063761, 0.086895, 0.123646),
    vec3(0.084817, 0.111994, 0.166380),
    vec3(0.097489, 0.154120, 0.091064),
    vec3(0.106152, 0.131144, 0.195191),
    vec3(0.097721, 0.110188, 0.187229),
    vec3(0.133516, 0.138278, 0.148582),
    vec3(0.070006, 0.243332, 0.235792),
    vec3(0.196766, 0.142899, 0.214696),
    vec3(0.047281, 0.315338, 0.321970),
    vec3(0.204675, 0.390010, 0.302066),
    vec3(0.080955, 0.314821, 0.661491)
);

const mat4 SCALE_TRANSLATE = mat4(
    0.5, 0.0, 0.0, 0.25,
    0.0, 0.5, 0.0, 0.25,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0
);

mat2 mat2_rotate_z(float radians) {
    return mat2(
        cos(radians), -sin(radians),
        sin(radians), cos(radians)
    );
}

mat4 end_portal_layer(float layer) {
    mat4 translate = mat4(
        1.0, 0.0, 0.0, 17.0 / layer,
        0.0, 1.0, 0.0, (2.0 + layer / 1.5) * (uTime * 1.5),
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    );

    mat2 rotate = mat2_rotate_z(radians((layer * layer * 4321.0 + layer * 9.0) * 2.0));

    mat2 scale = mat2((4.5 - layer / 4.0) * 2.0);

    return mat4(scale * rotate) * translate * SCALE_TRANSLATE;
}

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec3 color = textureProj(uSky, vTexProj).rgb * COLORS[0];
    for (int i = 0; i < PORTAL_LAYERS; i++) {
        color += textureProj(uPortal, vTexProj * end_portal_layer(float(i + 1))).rgb * COLORS[i];
    }
    vec3 fogDelta = vWorldPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    color = mix(color, uFogColor.rgb, fogValue * uFogColor.a);
    FragColor = vec4(color, 1.0);
}
)";

    namespace {
        // MC END_SKY_LOCATION / END_PORTAL_LOCATION
        // (AbstractEndPortalRenderer.java:20-21). Loaded STANDALONE rather
        // than through the block atlas: the shader tiles them with a
        // projective coordinate that runs far outside [0,1], which an atlas
        // sub-rect cannot express — packed into the atlas they would bleed
        // into their neighbours instead of repeating.
        //
        // Repeat + Nearest is what vanilla ends up with. Neither PNG ships a
        // .mcmeta, so TextureContents.blur()/clamp() are both false and
        // ReloadableTexture.apply (lines 26-30) picks AddressMode.REPEAT +
        // FilterMode.NEAREST for min and mag alike. Nearest is not an
        // oversight there: the layers are MINIFIED (each is scaled by up to
        // 9× in texture space), and the resulting per-pixel aliasing is the
        // portal's characteristic twinkle. Linear filtering smooths it into a
        // flat wash.
        TextureHandle LoadPortalTexture(const char* relativePath) {
            const std::string path = PlatformMain::GetAssetPath(relativePath);
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[EndPortalRenderer] stbi_load failed for %s: %s",
                             path.c_str(), stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(
                w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex,
                    TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(tex,
                    TextureWrap::Repeat, TextureWrap::Repeat);
            }
            return tex;
        }
    } // namespace

    EndPortalRenderer::~EndPortalRenderer() { Shutdown(); }

    bool EndPortalRenderer::Initialize() {
        if (!g_renderBackend) return false;

        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            // Vulkan needs the UBO-aware ("portal") pipeline layout: two
            // samplers (set=0 + set=2) and the Common UBO for uTime/fog.
            // CreateShaderFromFilesPortal is VKBackend-only — the abstract
            // RenderBackend has no such entry point — hence the cast, the
            // same shape XpOrbRenderer and PortalRenderer use.
            //
            // There is no inline-source fallback on this path:
            // VKBackend::CreateShader logs an error and returns
            // INVALID_SHADER for GLSL source, so calling it would only add
            // noise. Missing .spv → warn once here and stay disabled.
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal(
                PlatformMain::GetAssetPath("shaders/end_portal.vert"),
                PlatformMain::GetAssetPath("shaders/end_portal.frag"));
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                PlatformMain::GetAssetPath("shaders/end_portal.vert"),
                PlatformMain::GetAssetPath("shaders/end_portal.frag"));
            if (m_shader == INVALID_SHADER) {
                m_shader = g_renderBackend->CreateShader(s_vertSource, s_fragSource);
            }
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[EndPortalRenderer] failed to create shader — "
                         "end portals will not render");
            return false;
        }

        m_skyTexture    = LoadPortalTexture("assets/textures/environment/end_sky.png");
        m_portalTexture = LoadPortalTexture("assets/textures/entity/end_portal.png");
        if (m_skyTexture == INVALID_TEXTURE || m_portalTexture == INVALID_TEXTURE) {
            Log::Warning("[EndPortalRenderer] missing portal textures — disabled");
            Shutdown();
            return false;
        }

        // 12 verts per portal block (two faces × two triangles). A vanilla
        // portal is 9 blocks, so this covers several in view before the
        // buffer has to grow. One set per frame parity.
        for (FrameBuffers& fb : m_frames) {
            fb.capacityVerts = 12 * 64;
            fb.vb = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, fb.capacityVerts * sizeof(Vert),
                nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, INVALID_BUFFER, GetBlockVertexLayout());
            if (fb.vb == INVALID_BUFFER || fb.mesh == INVALID_MESH) {
                Log::Warning("[EndPortalRenderer] failed to create mesh — disabled");
                Shutdown();
                return false;
            }
        }

        m_initialized = true;
        Log::Info("[EndPortalRenderer] initialized");
        return true;
    }

    void EndPortalRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH)   { g_renderBackend->DestroyMesh(fb.mesh);   fb.mesh = INVALID_MESH; }
            if (fb.vb   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.vb);   fb.vb = INVALID_BUFFER; }
            fb.capacityVerts = 0;
        }
        if (m_skyTexture    != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_skyTexture);    m_skyTexture = INVALID_TEXTURE; }
        if (m_portalTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_portalTexture); m_portalTexture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_verts.clear();
        m_verts.shrink_to_fit();
        m_visibleChunks.clear();
        m_visibleChunks.shrink_to_fit();
        m_seen.clear();
        m_initialized = false;
    }

    void EndPortalRenderer::Render(Client::ClientChunkManager* chunkMgr,
                                   const glm::mat4& projection, const glm::mat4& view,
                                   const glm::vec3& cameraPos, float partialTick) {
        // Resource pack reload: both sheets are read again.
        if (m_initialized && g_renderBackend && Resources::CacheStale(m_packGeneration)) {
            if (m_skyTexture    != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_skyTexture);
            if (m_portalTexture != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_portalTexture);
            m_skyTexture    = LoadPortalTexture("assets/textures/environment/end_sky.png");
            m_portalTexture = LoadPortalTexture("assets/textures/entity/end_portal.png");
        }
        if (!m_initialized || !g_renderBackend || !chunkMgr) return;

        PROFILE_ZONE_N("EndPortals");

        // The same enumeration the block-entity dispatcher walks: only the
        // chunks with a section in this frame's draw list, each once. A
        // full walk of every loaded chunk was a second copy of the
        // dispatcher's per-frame cost, for a block that exists in one or
        // two chunks of a world.
        BlockEntityRenderDispatcher::CollectVisibleChunks(chunkMgr, m_visibleChunks, m_seen);
        const ChunkRenderer* sections = g_chunkRenderer;

        m_verts.clear();
        const float maxDistSq = kViewDistance * kViewDistance;

        // MC AbstractEndPortalRenderer.getOffsetDown/getOffsetUp (lines 60-66).
        // The portal is not a cube: only these two horizontal planes exist,
        // because shouldRenderFace returns true only for the Y axis
        // (TheEndPortalBlockEntity.shouldRenderFace).
        constexpr float kOffsetDown = 0.375f;
        constexpr float kOffsetUp   = 0.75f;

        // Quad → two triangles. UV and colour are inert (the shader reads
        // neither) but must be written: the vertex stride is the shared
        // 24-byte block layout.
        auto emitQuad = [this](const glm::vec3 (&q)[4]) {
            static const int kOrder[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i : kOrder) {
                m_verts.push_back({ q[i].x, q[i].y, q[i].z,
                                    0.0f, 0.0f, 255, 255, 255, 255 });
            }
        };

        for (const Client::ClientChunk* chunk : m_visibleChunks) {
            if (!chunk) continue;
            // Empty for all but a handful of chunks in a world — this is the
            // cull that actually matters, and it is a size() check.
            if (chunk->endPortals.empty() && chunk->endGateways.empty()) continue;

            for (const glm::ivec3& block : chunk->endPortals) {
                // The section gate, per block — MC's visibleSections walk
                // would never reach a portal in a section that is not drawn.
                if (sections &&
                    !sections->IsSectionVisible(chunk->position,
                                                (block.y - Config::MinY) >> 4)) {
                    continue;
                }
                const glm::vec3 base(block);
                // Distance cull against the block's centre, MC's cheap
                // horizontal test (BlockEntityRenderDispatcher does the same).
                const float dx = base.x + 0.5f - cameraPos.x;
                const float dz = base.z + 0.5f - cameraPos.z;
                if (dx * dx + dz * dz > maxDistSq) continue;

                // AbstractEndPortalRenderer.renderCube lines 46-47 through
                // renderFace lines 52-55. renderFace emits, in order,
                // (x1,y1,z1) (x2,y1,z2) (x2,y2,z3) (x1,y2,z4):
                //   DOWN → (0,d,0) (1,d,0) (1,d,1) (0,d,1)
                //   UP   → (0,u,1) (1,u,1) (1,u,0) (0,u,0)
                // Both windings are counter-clockwise seen from OUTSIDE the
                // block (down face from below, up face from above), which is
                // what makes back-face culling correct — get the order wrong
                // and the portal vanishes from one side.
                const glm::vec3 down[4] = {
                    base + glm::vec3(0.0f, kOffsetDown, 0.0f),
                    base + glm::vec3(1.0f, kOffsetDown, 0.0f),
                    base + glm::vec3(1.0f, kOffsetDown, 1.0f),
                    base + glm::vec3(0.0f, kOffsetDown, 1.0f),
                };
                const glm::vec3 up[4] = {
                    base + glm::vec3(0.0f, kOffsetUp, 1.0f),
                    base + glm::vec3(1.0f, kOffsetUp, 1.0f),
                    base + glm::vec3(1.0f, kOffsetUp, 0.0f),
                    base + glm::vec3(0.0f, kOffsetUp, 0.0f),
                };

                emitQuad(down);
                emitQuad(up);
            }

            // ── End gateways — the same starfield as a full cube ──────────
            //
            // MC TheEndGatewayRenderer draws all six faces of the block
            // (shouldRenderFace against the neighbours; the frame is bedrock,
            // so in practice the four sides plus whichever ends are open).
            // All six are emitted here — hidden faces lose the depth test
            // against the bedrock around them — inset a hair so an exposed
            // face cannot z-fight coplanar neighbour geometry.
            for (const glm::ivec3& block : chunk->endGateways) {
                if (sections &&
                    !sections->IsSectionVisible(chunk->position,
                                                (block.y - Config::MinY) >> 4)) {
                    continue;
                }
                const glm::vec3 base(block);
                const float dx = base.x + 0.5f - cameraPos.x;
                const float dz = base.z + 0.5f - cameraPos.z;
                if (dx * dx + dz * dz > maxDistSq) continue;

                constexpr float e0 = 0.001f;
                constexpr float e1 = 1.0f - 0.001f;
                // Each face wound counter-clockwise seen from OUTSIDE.
                const glm::vec3 gDown[4] = {
                    base + glm::vec3(e0, e0, e0), base + glm::vec3(e1, e0, e0),
                    base + glm::vec3(e1, e0, e1), base + glm::vec3(e0, e0, e1),
                };
                const glm::vec3 gUp[4] = {
                    base + glm::vec3(e0, e1, e1), base + glm::vec3(e1, e1, e1),
                    base + glm::vec3(e1, e1, e0), base + glm::vec3(e0, e1, e0),
                };
                const glm::vec3 gNorth[4] = {
                    base + glm::vec3(e1, e0, e0), base + glm::vec3(e0, e0, e0),
                    base + glm::vec3(e0, e1, e0), base + glm::vec3(e1, e1, e0),
                };
                const glm::vec3 gSouth[4] = {
                    base + glm::vec3(e0, e0, e1), base + glm::vec3(e1, e0, e1),
                    base + glm::vec3(e1, e1, e1), base + glm::vec3(e0, e1, e1),
                };
                const glm::vec3 gWest[4] = {
                    base + glm::vec3(e0, e0, e0), base + glm::vec3(e0, e0, e1),
                    base + glm::vec3(e0, e1, e1), base + glm::vec3(e0, e1, e0),
                };
                const glm::vec3 gEast[4] = {
                    base + glm::vec3(e1, e0, e1), base + glm::vec3(e1, e0, e0),
                    base + glm::vec3(e1, e1, e0), base + glm::vec3(e1, e1, e1),
                };
                emitQuad(gDown);
                emitQuad(gUp);
                emitQuad(gNorth);
                emitQuad(gSouth);
                emitQuad(gWest);
                emitQuad(gEast);
            }
        }

        if (m_verts.empty()) return;

        // This pass runs once per frame, so the parity flip IS the frame
        // boundary; the previous frame's set may still be in flight on
        // Vulkan, which is why there are two. See EntityFrame.hpp.
        m_frameCursor.Advance();
        FrameBuffers& fb = m_frames[m_frameCursor.parity];

        if (m_verts.size() > fb.capacityVerts) {
            size_t newCap = fb.capacityVerts;
            while (newCap < m_verts.size()) newCap *= 2;
            // Deferred: the frame that last drew from this set may still be
            // reading it. Immediate destroy here was a use-after-free on
            // Vulkan for the one frame a portal room first comes into view.
            g_renderBackend->DeferredDestroyMesh(fb.mesh);
            g_renderBackend->DeferredDestroyBuffer(fb.vb);
            fb.vb = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, newCap * sizeof(Vert),
                nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, INVALID_BUFFER, GetBlockVertexLayout());
            fb.capacityVerts = newCap;
            if (fb.vb == INVALID_BUFFER || fb.mesh == INVALID_MESH) {
                m_initialized = false;   // stop trying rather than draw garbage
                Log::Warning("[EndPortalRenderer] vertex buffer growth failed — disabled");
                return;
            }
        }
        g_renderBackend->UpdateBuffer(fb.vb, 0, m_verts.size() * sizeof(Vert), m_verts.data());

        // MC RenderPipelines.END_PORTAL_SNIPPET (RenderPipelines.java:154) +
        // END_PORTAL (line 211): no withBlend, no withDepthWrite(false), no
        // withCull(false) — so the defaults stand: opaque, depth-writing,
        // back-face culled.
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
        state.depthCompareOp    = CompareOp::LessEqual;
        state.blendEnabled      = false;
        state.cullMode          = CullMode::Back;
        state.frontFace         = FrontFace::CounterClockwise;
        state.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);

        g_renderBackend->BindShader(m_shader);

        // MC's GameTime: ((gameTime % 24000) + partialTick) / 24000
        // (GlobalSettingsUniform.java:30). The modulo is applied BEFORE the
        // partial tick is added, exactly as vanilla does — folding it after
        // would put a one-frame discontinuity at the wrap.
        const auto& envState = EnvironmentState::Get();
        const float gameTime = (static_cast<float>(envState.GameTime() % 24000) + partialTick)
                             / 24000.0f;
        g_renderBackend->SetUniformFloat(m_shader, "uTime", gameTime);

        g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * view);

        // Engine fog, in place of Mojang's apply_fog — same values the chunk
        // shaders get (ChunkRenderer::SetEnvironmentUniforms) so the portal
        // fades with the terrain it sits in.
        const EnvironmentFrame& env = envState.Frame();
        g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", cameraPos);
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(env.fogColor, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv",
            glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));

        // Sampler units. Ignored by Vulkan (textures bind via descriptor
        // sets), needed by OpenGL.
        g_renderBackend->SetUniformInt(m_shader, "uSky", 0);
        g_renderBackend->SetUniformInt(m_shader, "uPortal", 1);

        // Slot 1 first, slot 0 second: GLBackend::BindTexture leaves
        // glActiveTexture pointing at whichever unit it touched last, so
        // ending on slot 0 hands the next renderer the unit it expects.
        g_renderBackend->BindTexture(m_portalTexture, 1);
        g_renderBackend->BindTexture(m_skyTexture, 0);

        g_renderBackend->DrawArrays(fb.mesh, static_cast<uint32_t>(m_verts.size()));
        g_renderBackend->UnbindMesh();

        // Restore the default pipeline (mirrors every other standalone pass).
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

} // namespace Render
