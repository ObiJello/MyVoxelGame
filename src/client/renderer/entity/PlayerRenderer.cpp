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

    // Build stick-figure geometry for one player.
    // Body uses bodyYaw for orientation; head uses headYaw (independent look direction).
    // Crouching tilts the body forward along bodyForward.
    // Head has front face (bright, with smiley) and back face (dark fill), offset along
    // lookDir so depth testing separates them.
    static void BuildStickFigure(std::vector<StickVertex>& out,
                                 const glm::vec3& feetPos,
                                 float headYawDeg, float bodyYawDeg,
                                 float pitchDeg, bool isCrouching) {
        const uint8_t cr = 0, cg = 255, cb = 60, ca = 255;
        constexpr float PI = 3.14159265f;
        const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

        // Body orientation (torso, arms, legs)
        float bodyRad = glm::radians(bodyYawDeg);
        glm::vec3 bodyFwd{cosf(bodyRad), 0.0f, sinf(bodyRad)};
        glm::vec3 bodyRight = glm::normalize(glm::cross(bodyFwd, worldUp));

        // Head orientation (face direction)
        float headRad = glm::radians(headYawDeg);
        glm::vec3 lookDir{cosf(headRad), 0.0f, sinf(headRad)};
        glm::vec3 faceRight = glm::normalize(glm::cross(lookDir, worldUp));

        // Crouching offsets (Minecraft's HumanoidModel.java)
        float crouchTilt    = isCrouching ? 0.5f : 0.0f;
        float crouchHeadDrop = isCrouching ? (4.2f / 16.0f) : 0.0f;
        float crouchBodyDrop = isCrouching ? (3.2f / 16.0f) : 0.0f;
        float crouchLegBack  = isCrouching ? (4.0f / 16.0f) : 0.0f;

        // Key positions — body parts use bodyFwd/bodyRight
        float neckY  = 1.44f - crouchBodyDrop;
        float hipY   = 0.90f;
        float headCY = 1.62f - crouchHeadDrop;
        float handY  = 1.10f - crouchBodyDrop;

        glm::vec3 neck  = feetPos + worldUp * neckY  + bodyFwd * sinf(crouchTilt) * 0.3f;
        glm::vec3 hip   = feetPos + worldUp * hipY;
        glm::vec3 headC = feetPos + worldUp * headCY + bodyFwd * sinf(crouchTilt) * 0.35f;

        glm::vec3 footL = feetPos + bodyRight * (-0.20f) - bodyFwd * crouchLegBack;
        glm::vec3 footR = feetPos + bodyRight * ( 0.20f) - bodyFwd * crouchLegBack;

        glm::vec3 shoulderL = neck + bodyRight * (-0.05f);
        glm::vec3 shoulderR = neck + bodyRight * ( 0.05f);
        glm::vec3 handL = feetPos + worldUp * handY + bodyRight * (-0.35f) + bodyFwd * sinf(crouchTilt) * 0.2f;
        glm::vec3 handR = feetPos + worldUp * handY + bodyRight * ( 0.35f) + bodyFwd * sinf(crouchTilt) * 0.2f;

        // --- Body ---
        PushLine(out, neck, hip, cr, cg, cb, ca);

        // --- Legs ---
        PushLine(out, hip, footL, cr, cg, cb, ca);
        PushLine(out, hip, footR, cr, cg, cb, ca);

        // --- Arms ---
        PushLine(out, shoulderL, handL, cr, cg, cb, ca);
        PushLine(out, shoulderR, handR, cr, cg, cb, ca);

        // --- HEAD (uses lookDir/faceRight, NOT body orientation) ---
        float headRadius = 0.18f;
        float faceOffset = 0.04f; // depth offset to separate front/back

        // Front face: bright green circle + smiley, pushed forward along lookDir
        glm::vec3 frontC = headC + lookDir * faceOffset;
        PushCircle(out, frontC, faceRight, worldUp, headRadius, 16,
                   0.0f, 2.0f * PI, cr, cg, cb, ca);

        // Eyes (on front face)
        float eyeOffY = 0.04f, eyeOffX = 0.06f, eyeLen = 0.03f;
        glm::vec3 eyeL = frontC + worldUp * eyeOffY + faceRight * (-eyeOffX);
        glm::vec3 eyeR = frontC + worldUp * eyeOffY + faceRight * ( eyeOffX);
        PushLine(out, eyeL - faceRight * eyeLen, eyeL + faceRight * eyeLen, cr, cg, cb, ca);
        PushLine(out, eyeR - faceRight * eyeLen, eyeR + faceRight * eyeLen, cr, cg, cb, ca);

        // Smile (on front face)
        glm::vec3 mouthC = frontC - worldUp * 0.04f;
        PushCircle(out, mouthC, faceRight, worldUp, 0.07f, 8, PI, 2.0f * PI, cr, cg, cb, ca);

        // Back face: dark green circle + fill lines, pushed backward along lookDir
        const uint8_t br = 0, bg = 100, bb = 25, ba = 255;
        glm::vec3 backC = headC - lookDir * faceOffset;
        PushCircle(out, backC, faceRight, worldUp, headRadius, 16,
                   0.0f, 2.0f * PI, bg, bg, bb, ba);

        int backLines = 6;
        for (int i = 0; i < backLines; i++) {
            float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(backLines);
            float yOff = headRadius * (1.0f - 2.0f * t);
            float halfW = sqrtf(headRadius * headRadius - yOff * yOff);
            glm::vec3 lc = backC + worldUp * yOff;
            PushLine(out, lc + faceRight * (-halfW), lc + faceRight * halfW, br, bg, bb, ba);
        }
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

        // Build geometry for every remote player
        std::vector<StickVertex> verts;
        verts.reserve(players.size() * 120); // ~120 verts per figure (front+back head)

        for (const auto& [id, rp] : players) {
            float dx = rp.position.x - cameraPos.x;
            float dz = rp.position.z - cameraPos.z;
            if (dx * dx + dz * dz > 256.0f * 256.0f) continue;

            BuildStickFigure(verts, rp.position, rp.rotation.x, rp.bodyYaw, rp.rotation.y, rp.isCrouching);

            if (verts.size() + 120 > MAX_VERTICES) break;
        }

        if (verts.empty()) return;

        // Upload vertices
        g_renderBackend->UpdateBuffer(m_vb, 0, verts.size() * sizeof(StickVertex), verts.data());

        // Pipeline state: depth test + write on so front face occludes back of head
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
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
