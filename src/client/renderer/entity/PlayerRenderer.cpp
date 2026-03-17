// File: src/client/renderer/entity/PlayerRenderer.cpp
#include "PlayerRenderer.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <cmath>

namespace Render {

    // ------------------------------------------------------------------
    // Shader sources (OpenGL 330 core).
    // Uses the block vertex layout: pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2).
    // UV is unused; color carries the stick-figure colour.
    // ------------------------------------------------------------------

    const char* PlayerRenderer::s_vertSource = R"(
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
}
)";

    const char* PlayerRenderer::s_fragSource = R"(
#version 330 core

in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)";

    // ------------------------------------------------------------------
    // Vertex helper  (24 bytes, matches GetBlockVertexLayout)
    // ------------------------------------------------------------------

    struct StickVertex {
        float x, y, z;
        float u, v;
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(StickVertex) == 24, "StickVertex must match block vertex stride");

    // Push a single line segment as two vertices (drawn with PrimitiveType::Lines)
    static void PushLine(std::vector<StickVertex>& out,
                         const glm::vec3& a, const glm::vec3& b,
                         uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
        out.push_back({a.x, a.y, a.z, 0.0f, 0.0f, r, g, bl, al});
        out.push_back({b.x, b.y, b.z, 0.0f, 0.0f, r, g, bl, al});
    }

    // Push a circle (approximated by N line segments) in a plane defined by
    // center, right, and up vectors.
    static void PushCircle(std::vector<StickVertex>& out,
                           const glm::vec3& center, const glm::vec3& right,
                           const glm::vec3& up, float radius, int segments,
                           float startAngle, float endAngle,
                           uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
        float step = (endAngle - startAngle) / static_cast<float>(segments);
        for (int i = 0; i < segments; ++i) {
            float a0 = startAngle + step * static_cast<float>(i);
            float a1 = startAngle + step * static_cast<float>(i + 1);
            glm::vec3 p0 = center + right * (std::cos(a0) * radius) + up * (std::sin(a0) * radius);
            glm::vec3 p1 = center + right * (std::cos(a1) * radius) + up * (std::sin(a1) * radius);
            PushLine(out, p0, p1, r, g, bl, al);
        }
    }

    // Build stick-figure geometry for one player as a camera-facing billboard.
    // The figure is ~1.8 blocks tall (Minecraft player height).
    static void BuildStickFigure(std::vector<StickVertex>& out,
                                 const glm::vec3& feetPos,
                                 const glm::vec3& camRight,
                                 const glm::vec3& camUp) {
        // Colour: bright green for good contrast on any terrain
        const uint8_t cr = 0, cg = 255, cb = 60, ca = 255;

        // All measurements in blocks, billboarded via camRight / camUp.
        // Y-axis of the billboard is always world-up for readability.
        const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
        const glm::vec3& right = camRight;

        // Key points (relative to feet)
        const float headCenterY   = 1.62f;  // eye height
        const float headRadius    = 0.18f;
        const float neckY         = headCenterY - headRadius;
        const float hipY          = 0.90f;
        const float footSpreadX   = 0.20f;
        const float handSpreadX   = 0.35f;
        const float handY         = 1.10f;
        const float shoulderY     = neckY;

        // Positions in world space
        glm::vec3 neck     = feetPos + worldUp * neckY;
        glm::vec3 hip      = feetPos + worldUp * hipY;
        glm::vec3 headC    = feetPos + worldUp * headCenterY;
        glm::vec3 footL    = feetPos + right * (-footSpreadX);
        glm::vec3 footR    = feetPos + right * ( footSpreadX);
        glm::vec3 shoulderL= neck + right * (-0.05f);
        glm::vec3 shoulderR= neck + right * ( 0.05f);
        glm::vec3 handL    = feetPos + worldUp * handY + right * (-handSpreadX);
        glm::vec3 handR    = feetPos + worldUp * handY + right * ( handSpreadX);

        // --- Body ---
        PushLine(out, neck, hip, cr, cg, cb, ca);

        // --- Legs ---
        PushLine(out, hip, footL, cr, cg, cb, ca);
        PushLine(out, hip, footR, cr, cg, cb, ca);

        // --- Arms ---
        PushLine(out, shoulderL, handL, cr, cg, cb, ca);
        PushLine(out, shoulderR, handR, cr, cg, cb, ca);

        // --- Head circle (16 segments, full 360) ---
        PushCircle(out, headC, right, worldUp, headRadius, 16,
                   0.0f, 2.0f * 3.14159265f, cr, cg, cb, ca);

        // --- Eyes (two tiny dots as short horizontal lines) ---
        float eyeOffY  = 0.04f;
        float eyeOffX  = 0.06f;
        float eyeLen   = 0.03f;
        glm::vec3 eyeL = headC + worldUp * eyeOffY + right * (-eyeOffX);
        glm::vec3 eyeR = headC + worldUp * eyeOffY + right * ( eyeOffX);
        PushLine(out, eyeL - right * eyeLen, eyeL + right * eyeLen, cr, cg, cb, ca);
        PushLine(out, eyeR - right * eyeLen, eyeR + right * eyeLen, cr, cg, cb, ca);

        // --- Smile (half-circle, 8 segments, bottom half) ---
        float smileRadius = 0.07f;
        glm::vec3 mouthCenter = headC - worldUp * 0.04f;
        // Bottom half: pi to 2*pi (smile curves downward)
        PushCircle(out, mouthCenter, right, worldUp, smileRadius, 8,
                   3.14159265f, 2.0f * 3.14159265f, cr, cg, cb, ca);
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    PlayerRenderer::PlayerRenderer() = default;

    PlayerRenderer::~PlayerRenderer() {
        Shutdown();
    }

    bool PlayerRenderer::Initialize() {
        if (!g_renderBackend) {
            Log::Error("[PlayerRenderer] No render backend available");
            return false;
        }

        // Create shader (try SPIR-V first for Vulkan, fall back to source for GL)
        m_shader = g_renderBackend->CreateShaderFromFiles(
            "shaders/player_billboard.vert", "shaders/player_billboard.frag");
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShader(s_vertSource, s_fragSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Error("[PlayerRenderer] Failed to create shader");
            return false;
        }

        // 1x1 white dummy texture (required by Vulkan pipeline layout)
        unsigned char white[] = {255, 255, 255, 255};
        m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);

        // Dynamic vertex buffer -- re-uploaded each frame
        m_vb = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, MAX_VERTICES * sizeof(StickVertex), nullptr, BufferAccess::Streaming);

        // Mesh (no index buffer; we draw with DrawArrays using Lines)
        m_mesh = g_renderBackend->CreateMesh(m_vb, INVALID_BUFFER, GetBlockVertexLayout());

        Log::Info("[PlayerRenderer] Initialized");
        return true;
    }

    void PlayerRenderer::Shutdown() {
        if (!g_renderBackend) return;
        if (m_mesh != INVALID_MESH)           { g_renderBackend->DestroyMesh(m_mesh);         m_mesh = INVALID_MESH; }
        if (m_vb != INVALID_BUFFER)           { g_renderBackend->DestroyBuffer(m_vb);         m_vb = INVALID_BUFFER; }
        if (m_dummyTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_dummyTexture); m_dummyTexture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)       { g_renderBackend->DestroyShader(m_shader);     m_shader = INVALID_SHADER; }
    }

    // ------------------------------------------------------------------
    // Per-frame rendering
    // ------------------------------------------------------------------

    void PlayerRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                const glm::vec3& cameraPos,
                                const Client::RemotePlayerManager& remotePlayers) {
        if (m_shader == INVALID_SHADER || m_mesh == INVALID_MESH || !g_renderBackend) return;

        const auto& players = remotePlayers.GetPlayers();
        if (players.empty()) return;

        // Compute camera right vector from the view matrix
        // view rows: right = row0, up = row1, forward = -row2
        glm::vec3 camRight = glm::vec3(view[0][0], view[1][0], view[2][0]);

        // Build geometry for every remote player
        std::vector<StickVertex> verts;
        verts.reserve(players.size() * 80); // ~80 verts per figure

        for (const auto& [id, rp] : players) {
            // Skip players that are very far away (> 256 blocks)
            float dx = rp.position.x - cameraPos.x;
            float dz = rp.position.z - cameraPos.z;
            if (dx * dx + dz * dz > 256.0f * 256.0f) continue;

            BuildStickFigure(verts, rp.position, camRight, glm::vec3(0, 1, 0));

            if (verts.size() + 80 > MAX_VERTICES) break; // safety cap
        }

        if (verts.empty()) return;

        // Upload vertices
        g_renderBackend->UpdateBuffer(m_vb, 0, verts.size() * sizeof(StickVertex), verts.data());

        // Pipeline state: depth test on, no depth write, no blend, no cull, Lines
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = false;
        state.blendEnabled      = false;
        state.cullMode          = CullMode::None;
        state.primitiveType     = PrimitiveType::Lines;
        state.lineWidth         = 2.0f;
        g_renderBackend->SetPipelineState(state);

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_dummyTexture, 0);

        glm::mat4 mvp = projection * view; // model is identity (world space verts)
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);

        g_renderBackend->DrawArrays(m_mesh, static_cast<uint32_t>(verts.size()));

        g_renderBackend->UnbindMesh();

        // Restore default pipeline state
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        defaultState.polygonMode       = PolygonMode::Fill;
        g_renderBackend->SetPipelineState(defaultState);
    }

} // namespace Render
