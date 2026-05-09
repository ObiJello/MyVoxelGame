// File: src/client/renderer/entity/PlayerRenderer.cpp
#include "PlayerRenderer.hpp"
#include "StickFigureGeometry.hpp"
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
    // Convert line pairs into camera-facing thick triangle strips with a
    // FIXED WORLD-SPACE width. Each line (a,b) becomes a quad whose two long
    // edges are (a,b) and the perpendicular `cross(b-a, camera-midpoint)`
    // gives the strip direction (always faces the camera). Width is in world
    // metres → perspective shrinks far players naturally; close players have
    // visibly thicker limbs.
    // ------------------------------------------------------------------
    static void EmitThickWorldStripFromLines(const std::vector<StickVertex>& lineVerts,
                                             const glm::vec3& cameraPos,
                                             float halfWidth,
                                             std::vector<StickVertex>& triOut) {
        triOut.reserve(triOut.size() + (lineVerts.size() / 2) * 6);
        for (size_t i = 0; i + 1 < lineVerts.size(); i += 2) {
            const auto& va = lineVerts[i];
            const auto& vb = lineVerts[i + 1];
            glm::vec3 a(va.x, va.y, va.z);
            glm::vec3 b(vb.x, vb.y, vb.z);
            glm::vec3 d = b - a;
            float dLen2 = glm::dot(d, d);
            if (dLen2 < 1e-12f) continue;
            glm::vec3 mid = (a + b) * 0.5f;
            glm::vec3 toCam = cameraPos - mid;
            glm::vec3 perp = glm::cross(d, toCam);
            float pLen2 = glm::dot(perp, perp);
            if (pLen2 < 1e-12f) continue; // line points directly at camera
            perp = glm::normalize(perp) * halfWidth;

            // Extend each endpoint along the line direction by the "miter
            // factor" — exactly halfWidth * tan(angleChange/2) — so adjacent
            // chained segments meet cleanly with no outer gap and no visible
            // diamond spike past the curve. StickFigureGeometry's 64-segment
            // head circle and 32-segment half-smile both share a 5.625° per-
            // segment angle change → tan(2.8125°) ≈ 0.049 is correct for both.
            constexpr float kMiterFactor = 0.049f;
            glm::vec3 along = glm::normalize(d) * (halfWidth * kMiterFactor);
            glm::vec3 ae = a - along; // slightly pulled-back start
            glm::vec3 be = b + along; // slightly pushed-forward end

            glm::vec3 a0 = ae + perp, a1 = ae - perp;
            glm::vec3 b0 = be + perp, b1 = be - perp;

            auto push = [&](const glm::vec3& p) {
                triOut.push_back({p.x, p.y, p.z, 0.0f, 0.0f, va.r, va.g, va.b, va.a});
            };
            // Two triangles forming the camera-facing quad (no culling — both
            // sides should be visible if the camera flips around).
            push(a0); push(a1); push(b1);
            push(a0); push(b1); push(b0);
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

            const auto& colorEntry = Game::LookupPlayerColor(rp.color);
            PlayerColor color{ colorEntry.r, colorEntry.g, colorEntry.b, 255 };
            BuildStickFigure(lineVerts, triVerts, rp.position,
                             rp.rotation.x, rp.bodyYaw, rp.rotation.y, rp.isCrouching,
                             color);
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

        // --- Pass 2: Body/limbs/head outline/face features as camera-facing
        // thick triangle strips with FIXED WORLD-SPACE width. Replaces the old
        // PrimitiveType::Lines path so close players have visibly thick limbs
        // and far players naturally shrink via perspective (instead of the
        // GPU's always-1-logical-pixel rendering).
        if (!lineVerts.empty() && m_lineMesh != INVALID_MESH) {
            // Width tuned so a player at ~5 m distance has limbs that read as
            // "stick-figure thick" (~1 px on a 1080p frame at default FOV).
            // Closer ⇒ thicker via perspective; farther ⇒ thinner.
            constexpr float kStripHalfWidth = 0.018f; // 1.8 cm half-width = 3.6 cm full

            std::vector<StickVertex> stripVerts;
            EmitThickWorldStripFromLines(lineVerts, cameraPos, kStripHalfWidth, stripVerts);

            if (!stripVerts.empty() && stripVerts.size() <= MAX_VERTICES) {
                g_renderBackend->UpdateBuffer(m_lineVB, 0,
                    stripVerts.size() * sizeof(StickVertex), stripVerts.data());

                PipelineState stripState;
                stripState.depthTestEnabled  = true;
                stripState.depthWriteEnabled = true;
                stripState.blendEnabled      = false;
                stripState.cullMode          = CullMode::None;        // strips face camera; both sides visible
                stripState.primitiveType     = PrimitiveType::Triangles;
                g_renderBackend->SetPipelineState(stripState);

                g_renderBackend->BindShader(m_shader);
                g_renderBackend->BindTexture(m_dummyTexture, 0);
                g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
                g_renderBackend->DrawArrays(m_lineMesh, static_cast<uint32_t>(stripVerts.size()));
                g_renderBackend->UnbindMesh();
            }
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
