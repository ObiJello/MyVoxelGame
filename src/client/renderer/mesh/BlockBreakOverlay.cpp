// File: src/client/renderer/mesh/BlockBreakOverlay.cpp
#include "BlockBreakOverlay.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <string>
#include <vector>

namespace Render {

    BlockBreakOverlay g_blockBreakOverlay;

    // Vertex layout matches GetBlockVertexLayout (pos3 + uv2 + color4 ubyte).
    // The cube is in local space [0,1]³; we translate to the target world
    // position via the model matrix on Render.
    //
    // UVs are NORMALIZED into the sub-rect [0,1] — the fragment shader maps
    // them into the atlas sub-rect for the current stage via uUvMin/uUvMax.
    const char* BlockBreakOverlay::vertexShaderSource = R"(
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
    vUV = mix(uUVMin, uUVMax, aUV);
}
)";

    const char* BlockBreakOverlay::fragmentShaderSource = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAtlas;
void main() {
    vec4 t = texture(uAtlas, vUV);
    // MC's "crumbling" pass: blendFuncSeparate(DST_COLOR, SRC_COLOR, ONE, ZERO).
    // Formula: out.rgb = src.rgb * dst.rgb + dst.rgb * src.rgb = 2 * src * dst.
    // The destroy_stage_X.png is grey cracks on a transparent background, so:
    //   • At non-crack pixels (alpha = 0) we MUST discard — otherwise the
    //     2 * 0 * dst = 0 would punch a black hole in the block.
    //   • At crack pixels the grey RGB multiplicatively darkens the block,
    //     preserving its underlying texture/color (vs. drawing flat black).
    if (t.a < 0.05) discard;
    FragColor = vec4(t.rgb, 1.0);
}
)";

    BlockBreakOverlay::~BlockBreakOverlay() { Shutdown(); }

    void BlockBreakOverlay::Shutdown() {
        if (!g_renderBackend) return;
        if (m_mesh   != INVALID_MESH)   { g_renderBackend->DestroyMesh(m_mesh);     m_mesh = INVALID_MESH; }
        if (m_vb     != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_vb);     m_vb = INVALID_BUFFER; }
        if (m_ib     != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_ib);     m_ib = INVALID_BUFFER; }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
    }

    bool BlockBreakOverlay::Initialize() {
        if (!g_renderBackend) {
            Log::Error("BlockBreakOverlay: no render backend");
            return false;
        }

        // Try SPIR-V (Vulkan), then fall back to GLSL source (OpenGL).
        m_shader = g_renderBackend->CreateShaderFromFiles(
            "shaders/block_break_overlay.vert", "shaders/block_break_overlay.frag");
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShader(vertexShaderSource, fragmentShaderSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Error("BlockBreakOverlay: failed to create shader");
            return false;
        }

        // Unit cube — 6 faces × 4 verts × (pos3 + uv2 + color4 ubyte).
        // Per-face UVs span [0,1]² in NORMALIZED space; the vertex shader
        // maps those into the atlas sub-rect.
        struct V { float x, y, z; float u, v; uint8_t r,g,b,a; };
        static_assert(sizeof(V) == 24, "vertex stride must match block layout");
        const uint8_t W = 255;
        std::vector<V> verts;
        std::vector<uint32_t> idx;
        verts.reserve(24);
        idx.reserve(36);

        auto addFace = [&](const glm::vec3& p0, const glm::vec3& p1,
                           const glm::vec3& p2, const glm::vec3& p3) {
            uint32_t base = static_cast<uint32_t>(verts.size());
            verts.push_back({p0.x, p0.y, p0.z, 0.0f, 1.0f, W,W,W,W});
            verts.push_back({p1.x, p1.y, p1.z, 1.0f, 1.0f, W,W,W,W});
            verts.push_back({p2.x, p2.y, p2.z, 1.0f, 0.0f, W,W,W,W});
            verts.push_back({p3.x, p3.y, p3.z, 0.0f, 0.0f, W,W,W,W});
            idx.insert(idx.end(), {base+0, base+1, base+2, base+0, base+2, base+3});
        };

        // 6 faces of the unit cube, CCW-from-outside winding so back-face
        // culling shows them. Winding doesn't matter much here since we
        // disable culling for the overlay, but keep it correct anyway.
        // -Z (north)
        addFace({1,0,0}, {0,0,0}, {0,1,0}, {1,1,0});
        // +Z (south)
        addFace({0,0,1}, {1,0,1}, {1,1,1}, {0,1,1});
        // -X (west)
        addFace({0,0,0}, {0,0,1}, {0,1,1}, {0,1,0});
        // +X (east)
        addFace({1,0,1}, {1,0,0}, {1,1,0}, {1,1,1});
        // -Y (bottom)
        addFace({0,0,0}, {1,0,0}, {1,0,1}, {0,0,1});
        // +Y (top)
        addFace({0,1,1}, {1,1,1}, {1,1,0}, {0,1,0});

        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
            verts.size() * sizeof(V), verts.data());
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
            idx.size() * sizeof(uint32_t), idx.data());
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        Log::Info("BlockBreakOverlay initialized");
        return true;
    }

    bool BlockBreakOverlay::ResolveAtlas() {
        if (!g_atlasBuilder) return false;
        m_atlasTexture = g_atlasBuilder->GetBackendTextureHandle();
        if (m_atlasTexture == INVALID_TEXTURE) return false;
        bool ok = true;
        for (int i = 0; i < 10; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "block/destroy_stage_%d", i);
            if (!g_atlasBuilder->GetUVRect(buf, m_stageUVs[i])) {
                Log::Warning("BlockBreakOverlay: missing atlas sprite '%s'", buf);
                ok = false;
            }
        }
        m_uvsResolved = ok;
        return ok;
    }

    void BlockBreakOverlay::SetTarget(const glm::ivec3& pos, int stage,
                                      const glm::vec3& shapeMin,
                                      const glm::vec3& shapeMax) {
        m_pos = pos;
        m_stage = (stage < 0) ? -1 : (stage > 9 ? 9 : stage);
        m_shapeMin = shapeMin;
        m_shapeMax = shapeMax;
    }

    void BlockBreakOverlay::Render(const glm::mat4& projectionMatrix,
                                   const glm::mat4& viewMatrix) {
        if (m_stage < 0 || m_shader == INVALID_SHADER || m_mesh == INVALID_MESH
            || !g_renderBackend) return;

        // Lazy atlas resolution — atlas isn't ready when Initialize runs.
        if (!m_uvsResolved && !ResolveAtlas()) return;

        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = false;
        state.depthCompareOp    = CompareOp::LessEqual;
        // MC "crumbling" blend: out = 2 * src * dst — multiplicatively
        // darkens the underlying block instead of overlaying flat black.
        state.blendEnabled      = true;
        state.srcBlendFactor    = BlendFactor::DstColor;
        state.dstBlendFactor    = BlendFactor::SrcColor;
        state.cullMode          = CullMode::None;
        state.primitiveType     = PrimitiveType::Triangles;
        // Push the overlay forward in depth so it survives Z-fight against
        // the underlying chunk mesh at distance.
        state.depthBiasEnabled  = true;
        state.depthBiasConstant = -2.0f;
        state.depthBiasSlope    = -2.0f;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_atlasTexture, 0);

        // Translate to the block, then scale the unit-cube mesh by the shape
        // bounds so partial blocks (leaf litter, slabs, fences, …) get the
        // crack wrapped on their real surface instead of a floating cube.
        const glm::vec3 size = m_shapeMax - m_shapeMin;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(m_pos) + m_shapeMin);
        model = glm::scale(model, size);
        glm::mat4 mvp = projectionMatrix * viewMatrix * model;
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        // "uUVMin"/"uUVMax" exactly — VKBackend::SetUniformVec2 dispatches on
        // the literal name to pick a push-constant slot, and silently drops
        // anything it doesn't recognise. The old "uUvMin"/"uUvMax" spelling
        // matched no branch, so Vulkan drew the overlay with a stale UV range
        // and the cracks never showed (GL resolves uniforms by name, so it
        // worked there). Keep these in sync with the GLSL above, the SPIR-V in
        // shaders/block_break_overlay_vk.vert, and the backend's dispatch.
        g_renderBackend->SetUniformVec2(m_shader, "uUVMin", m_stageUVs[m_stage].uvMin);
        g_renderBackend->SetUniformVec2(m_shader, "uUVMax", m_stageUVs[m_stage].uvMax);

        g_renderBackend->DrawIndexed(m_mesh, 36);
        g_renderBackend->UnbindMesh();

        // Restore default pipeline state (mirror BlockHighlight pattern).
        PipelineState def;
        def.depthTestEnabled  = true;
        def.depthWriteEnabled = true;
        def.blendEnabled      = false;
        def.cullMode          = CullMode::Back;
        def.polygonMode       = PolygonMode::Fill;
        g_renderBackend->SetPipelineState(def);
    }

} // namespace Render
