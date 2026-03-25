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

    static void PushLine(std::vector<StickVertex>& out,
                         const glm::vec3& a, const glm::vec3& b,
                         uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
        out.push_back({a.x, a.y, a.z, 0.0f, 0.0f, r, g, bl, al});
        out.push_back({b.x, b.y, b.z, 0.0f, 0.0f, r, g, bl, al});
    }

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

    // Push a filled disc as a triangle fan. The disc normal faces along `normal`.
    // With CullMode::Back, the disc is only visible from the side `normal` points at.
    static void PushDisc(std::vector<StickVertex>& out,
                         const glm::vec3& center, const glm::vec3& right,
                         const glm::vec3& up, const glm::vec3& normal,
                         float radius, int segments,
                         uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
        constexpr float TWO_PI = 2.0f * 3.14159265f;
        float step = TWO_PI / static_cast<float>(segments);

        // Determine winding: if normal points toward camera, wind CCW (default front face).
        // We want the disc visible from the normal side, so wind CCW when looking along -normal.
        for (int i = 0; i < segments; ++i) {
            float a0 = step * static_cast<float>(i);
            float a1 = step * static_cast<float>(i + 1);
            glm::vec3 p0 = center + right * (cosf(a0) * radius) + up * (sinf(a0) * radius);
            glm::vec3 p1 = center + right * (cosf(a1) * radius) + up * (sinf(a1) * radius);
            // Triangle: center, p0, p1 (CCW when viewed from normal direction)
            out.push_back({center.x, center.y, center.z, 0.0f, 0.0f, r, g, bl, al});
            out.push_back({p0.x, p0.y, p0.z, 0.0f, 0.0f, r, g, bl, al});
            out.push_back({p1.x, p1.y, p1.z, 0.0f, 0.0f, r, g, bl, al});
        }
    }

    // ------------------------------------------------------------------
    // Stick-figure builder
    // ------------------------------------------------------------------

    // Build line geometry (body, limbs, head outline, face features) into lineVerts,
    // and triangle geometry (filled back-of-head disc) into triVerts.
    static void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                                 std::vector<StickVertex>& triVerts,
                                 const glm::vec3& feetPos,
                                 float headYawDeg, float bodyYawDeg,
                                 float pitchDeg, bool isCrouching) {
        const uint8_t cr = 0, cg = 255, cb = 60, ca = 255;
        constexpr float PI = 3.14159265f;
        const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

        // Body orientation
        float bodyRad = glm::radians(bodyYawDeg);
        glm::vec3 bodyFwd{cosf(bodyRad), 0.0f, sinf(bodyRad)};
        glm::vec3 bodyRight = glm::normalize(glm::cross(bodyFwd, worldUp));

        // Head orientation
        float headRad = glm::radians(headYawDeg);
        glm::vec3 lookDir{cosf(headRad), 0.0f, sinf(headRad)};
        glm::vec3 faceRight = glm::normalize(glm::cross(lookDir, worldUp));

        // Crouching (Minecraft's HumanoidModel.java)
        float crouchTilt     = isCrouching ? 0.5f : 0.0f;
        float crouchHeadDrop = isCrouching ? (4.2f / 16.0f) : 0.0f;
        float crouchBodyDrop = isCrouching ? (3.2f / 16.0f) : 0.0f;
        float crouchLegBack  = isCrouching ? (4.0f / 16.0f) : 0.0f;

        float neckY  = 1.44f - crouchBodyDrop;
        float hipY   = 0.90f;
        float headCY = 1.62f - crouchHeadDrop;
        float handY  = 1.10f - crouchBodyDrop;

        glm::vec3 neck  = feetPos + worldUp * neckY  + bodyFwd * sinf(crouchTilt) * 0.3f;
        glm::vec3 hip   = feetPos + worldUp * hipY;
        glm::vec3 headC = feetPos + worldUp * headCY + bodyFwd * sinf(crouchTilt) * 0.35f;

        glm::vec3 footL = feetPos + bodyRight * (-0.20f) - bodyFwd * crouchLegBack;
        glm::vec3 footR = feetPos + bodyRight * ( 0.20f) - bodyFwd * crouchLegBack;
        glm::vec3 shoulderPos = neck - worldUp * 0.14f; // slightly below neck
        glm::vec3 shoulderL = shoulderPos + bodyRight * (-0.05f);
        glm::vec3 shoulderR = shoulderPos + bodyRight * ( 0.05f);
        glm::vec3 handL = feetPos + worldUp * handY + bodyRight * (-0.35f) + bodyFwd * sinf(crouchTilt) * 0.2f;
        glm::vec3 handR = feetPos + worldUp * handY + bodyRight * ( 0.35f) + bodyFwd * sinf(crouchTilt) * 0.2f;

        // --- LINES: Body, legs, arms ---
        PushLine(lineVerts, neck, hip, cr, cg, cb, ca);
        PushLine(lineVerts, hip, footL, cr, cg, cb, ca);
        PushLine(lineVerts, hip, footR, cr, cg, cb, ca);
        PushLine(lineVerts, shoulderL, handL, cr, cg, cb, ca);
        PushLine(lineVerts, shoulderR, handR, cr, cg, cb, ca);

        // --- LINES: Front head outline + face features ---
        float headRadius = 0.18f;
        glm::vec3 frontC = headC;

        PushCircle(lineVerts, frontC, faceRight, worldUp, headRadius, 16,
                   0.0f, 2.0f * PI, cr, cg, cb, ca);

        // Eyes
        float eyeOffY = 0.04f, eyeOffX = 0.06f, eyeLen = 0.03f;
        glm::vec3 eyeL = frontC + worldUp * eyeOffY + faceRight * (-eyeOffX);
        glm::vec3 eyeR = frontC + worldUp * eyeOffY + faceRight * ( eyeOffX);
        PushLine(lineVerts, eyeL - faceRight * eyeLen, eyeL + faceRight * eyeLen, cr, cg, cb, ca);
        PushLine(lineVerts, eyeR - faceRight * eyeLen, eyeR + faceRight * eyeLen, cr, cg, cb, ca);

        // Smile
        glm::vec3 mouthC = frontC - worldUp * 0.04f;
        PushCircle(lineVerts, mouthC, faceRight, worldUp, 0.07f, 8, PI, 2.0f * PI, cr, cg, cb, ca);

        // --- TRIANGLES: Back-of-head filled disc (GPU face-culled) ---
        // Placed at headC (no offset) so it lines up with the neck/body connection.
        // Front features are offset forward, so they still render in front of this disc.
        PushDisc(triVerts, headC, faceRight, worldUp, -lookDir,
                 headRadius, 16, cr, cg, cb, ca);
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

        m_shader = g_renderBackend->CreateShaderFromFiles(
            "shaders/player_billboard.vert", "shaders/player_billboard.frag");
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShader(s_vertSource, s_fragSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Error("[PlayerRenderer] Failed to create shader");
            return false;
        }

        unsigned char white[] = {255, 255, 255, 255};
        m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);

        // Two vertex buffers: one for lines, one for triangles
        m_lineVB = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, MAX_VERTICES * sizeof(StickVertex), nullptr, BufferAccess::Streaming);
        m_lineMesh = g_renderBackend->CreateMesh(m_lineVB, INVALID_BUFFER, GetBlockVertexLayout());

        m_triVB = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, MAX_VERTICES * sizeof(StickVertex), nullptr, BufferAccess::Streaming);
        m_triMesh = g_renderBackend->CreateMesh(m_triVB, INVALID_BUFFER, GetBlockVertexLayout());

        Log::Info("[PlayerRenderer] Initialized");
        return true;
    }

    void PlayerRenderer::Shutdown() {
        if (!g_renderBackend) return;
        if (m_lineMesh != INVALID_MESH)       { g_renderBackend->DestroyMesh(m_lineMesh);       m_lineMesh = INVALID_MESH; }
        if (m_lineVB != INVALID_BUFFER)       { g_renderBackend->DestroyBuffer(m_lineVB);       m_lineVB = INVALID_BUFFER; }
        if (m_triMesh != INVALID_MESH)        { g_renderBackend->DestroyMesh(m_triMesh);        m_triMesh = INVALID_MESH; }
        if (m_triVB != INVALID_BUFFER)        { g_renderBackend->DestroyBuffer(m_triVB);        m_triVB = INVALID_BUFFER; }
        if (m_dummyTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_dummyTexture); m_dummyTexture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)       { g_renderBackend->DestroyShader(m_shader);       m_shader = INVALID_SHADER; }
    }

    // ------------------------------------------------------------------
    // Per-frame rendering
    // ------------------------------------------------------------------

    void PlayerRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                const glm::vec3& cameraPos,
                                const Client::RemotePlayerManager& remotePlayers) {
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;

        const auto& players = remotePlayers.GetPlayers();
        if (players.empty()) return;

        std::vector<StickVertex> lineVerts;
        std::vector<StickVertex> triVerts;
        lineVerts.reserve(players.size() * 80);
        triVerts.reserve(players.size() * 48); // 16 segments * 3 verts per tri

        for (const auto& [id, rp] : players) {
            float dx = rp.position.x - cameraPos.x;
            float dz = rp.position.z - cameraPos.z;
            if (dx * dx + dz * dz > 256.0f * 256.0f) continue;

            BuildStickFigure(lineVerts, triVerts, rp.position,
                             rp.rotation.x, rp.bodyYaw, rp.rotation.y, rp.isCrouching);
        }

        glm::mat4 mvp = projection * view;

        // --- Pass 1: Filled back-of-head discs (triangles, back-face culled) ---
        if (!triVerts.empty() && m_triMesh != INVALID_MESH) {
            g_renderBackend->UpdateBuffer(m_triVB, 0, triVerts.size() * sizeof(StickVertex), triVerts.data());

            PipelineState triState;
            triState.depthTestEnabled  = true;
            triState.depthWriteEnabled = true;
            triState.blendEnabled      = false;
            triState.cullMode          = CullMode::Back;       // only show front-facing tris
            triState.frontFace         = FrontFace::CounterClockwise;
            triState.primitiveType     = PrimitiveType::Triangles;
            g_renderBackend->SetPipelineState(triState);

            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_dummyTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->DrawArrays(m_triMesh, static_cast<uint32_t>(triVerts.size()));
            g_renderBackend->UnbindMesh();
        }

        // --- Pass 2: Lines (body, limbs, head outline, face features) ---
        if (!lineVerts.empty() && m_lineMesh != INVALID_MESH) {
            g_renderBackend->UpdateBuffer(m_lineVB, 0, lineVerts.size() * sizeof(StickVertex), lineVerts.data());

            PipelineState lineState;
            lineState.depthTestEnabled  = true;
            lineState.depthWriteEnabled = true;
            lineState.blendEnabled      = false;
            lineState.cullMode          = CullMode::None;      // lines have no face
            lineState.primitiveType     = PrimitiveType::Lines;
            lineState.lineWidth         = 2.0f;
            g_renderBackend->SetPipelineState(lineState);

            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_dummyTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->DrawArrays(m_lineMesh, static_cast<uint32_t>(lineVerts.size()));
            g_renderBackend->UnbindMesh();
        }

        // Restore default
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        defaultState.polygonMode       = PolygonMode::Fill;
        g_renderBackend->SetPipelineState(defaultState);
    }

    void PlayerRenderer::RenderChatBubbles(const glm::mat4& projection, const glm::mat4& view,
                                           const Client::RemotePlayerManager& remotePlayers,
                                           int fbWidth, int fbHeight) {
        if (!g_renderBackend || fbWidth <= 0 || fbHeight <= 0) return;

        glm::mat4 vp = projection * view;
        glm::mat4 ortho = glm::ortho(0.0f, static_cast<float>(fbWidth),
                                      static_cast<float>(fbHeight), 0.0f, -1.0f, 1.0f);

        for (const auto& [id, rp] : remotePlayers.GetPlayers()) {
            if (rp.chatBubbleTimer <= 0.0f || rp.chatBubbleText.empty()) continue;

            // Project player head position to screen
            glm::vec4 worldPos(rp.position.x, rp.position.y + 2.4f, rp.position.z, 1.0f);
            glm::vec4 clip = vp * worldPos;
            if (clip.w <= 0.0f) continue;

            float ndcX = clip.x / clip.w;
            float ndcY = clip.y / clip.w;
            float screenX = (ndcX * 0.5f + 0.5f) * fbWidth;
            float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * fbHeight;

            // Approximate text width (6px per char)
            const std::string& text = rp.chatBubbleText;
            int textWidth = static_cast<int>(text.size()) * 6;
            int padding = 5;
            int bubbleW = textWidth + padding * 2;
            int bubbleH = 14;
            int bx = static_cast<int>(screenX) - bubbleW / 2;
            int by = static_cast<int>(screenY) - bubbleH - 8;

            PipelineState state;
            state.depthTestEnabled = false;
            state.depthWriteEnabled = false;
            state.blendEnabled = true;
            state.srcBlendFactor = BlendFactor::SrcAlpha;
            state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
            state.cullMode = CullMode::None;
            g_renderBackend->SetPipelineState(state);
            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_dummyTexture, 0);

            struct V { float x,y,z,u,v; uint8_t r,g,b,a; };
            auto drawRect = [&](int x0, int y0, int x1, int y1, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
                V verts[] = {
                    {(float)x0,(float)y0,0,0,0,cr,cg,cb,ca},
                    {(float)x1,(float)y0,0,0,0,cr,cg,cb,ca},
                    {(float)x1,(float)y1,0,0,0,cr,cg,cb,ca},
                    {(float)x0,(float)y1,0,0,0,cr,cg,cb,ca},
                };
                uint32_t idx[] = {0,1,2,0,2,3};
                auto vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, sizeof(verts), verts);
                auto ib = g_renderBackend->CreateBuffer(BufferUsage::Index, sizeof(idx), idx);
                auto mesh = g_renderBackend->CreateMesh(vb, ib, GetBlockVertexLayout());
                g_renderBackend->SetUniformMat4(m_shader, "uMVP", ortho);
                g_renderBackend->DrawIndexed(mesh, 6);
                g_renderBackend->UnbindMesh();
                g_renderBackend->DestroyMesh(mesh);
                g_renderBackend->DestroyBuffer(vb);
                g_renderBackend->DestroyBuffer(ib);
            };

            // Black outline
            drawRect(bx-1, by-1, bx+bubbleW+1, by+bubbleH+1, 0,0,0,255);
            // White fill
            drawRect(bx, by, bx+bubbleW, by+bubbleH, 255,255,255,255);
            // Triangle pointer
            int tx = static_cast<int>(screenX);
            int ty = by + bubbleH;
            drawRect(tx-3, ty, tx+3, ty+1, 0,0,0,255);
            drawRect(tx-2, ty+1, tx+2, ty+2, 0,0,0,255);
            drawRect(tx-1, ty+2, tx+1, ty+3, 0,0,0,255);
            drawRect(tx-2, ty, tx+2, ty+1, 255,255,255,255);
            drawRect(tx-1, ty+1, tx+1, ty+2, 255,255,255,255);

            PipelineState def;
            def.depthTestEnabled = true;
            def.depthWriteEnabled = true;
            def.blendEnabled = false;
            def.cullMode = CullMode::Back;
            g_renderBackend->SetPipelineState(def);
        }
    }

} // namespace Render
