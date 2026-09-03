// File: src/client/renderer/entity/PlayerRenderer.cpp
#include "PlayerRenderer.hpp"
#include "StickFigureGeometry.hpp"
#include "EntityCulling.hpp"
#include "client/world/ClientLevel.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "MobRenderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <algorithm>
#include <cmath>

namespace Render {

    namespace {
        // MC LivingEntityRenderer.setupRotations' death topple, applied to the
        // stick figure AFTER it has been baked into world space.
        //
        // The geometry builder emits absolute positions rather than a posed
        // model, so the roll cannot be composed into a model matrix the way the
        // mob renderer does it — it is applied here instead, about the feet and
        // around the body's own forward axis, which is what makes the corpse
        // fall sideways relative to the way it was facing rather than always
        // toward one compass direction.
        // The body size, about the feet — the same transform the local
        // player's BodyScaleModel applies, baked into this player's slice.
        void ScaleStickFigure(std::vector<StickVertex>& verts, size_t begin,
                              const glm::vec3& feet, float scale) {
            if (std::abs(scale - 1.0f) < 1e-4f) return;
            for (size_t i = begin; i < verts.size(); ++i) {
                verts[i].x = feet.x + (verts[i].x - feet.x) * scale;
                verts[i].y = feet.y + (verts[i].y - feet.y) * scale;
                verts[i].z = feet.z + (verts[i].z - feet.z) * scale;
            }
        }

        void ToppleStickFigure(std::vector<StickVertex>& verts, size_t begin,
                               const glm::vec3& feet, float bodyYawDeg,
                               float flipDeg) {
            if (flipDeg == 0.0f) return;
            const glm::vec3 axis = Game::Mth::HorizontalViewVector(bodyYawDeg);
            const glm::mat4 rot =
                glm::rotate(glm::mat4(1.0f), glm::radians(flipDeg), axis);
            for (size_t i = begin; i < verts.size(); ++i) {
                const glm::vec3 p(verts[i].x, verts[i].y, verts[i].z);
                const glm::vec3 q =
                    feet + glm::vec3(rot * glm::vec4(p - feet, 1.0f));
                verts[i].x = q.x;
                verts[i].y = q.y;
                verts[i].z = q.z;
            }
        }
    } // namespace

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
// Optional pre-transform applied to the world-space stick-figure verts.
// Identity for normal player rendering; for portal ghost passes this is
// the source-to-destination portal pair matrix M, so the player appears
// emerging from the destination portal in its mirrored pose.
uniform mat4 uModel;

out vec3 vWorldPos;
out vec4 vColor;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos    = worldPos.xyz;
    gl_Position  = uMVP * worldPos;
    vColor       = aColor;
}
)";

    const char* PlayerRenderer::s_fragSource = R"(
#version 330 core

in vec3 vWorldPos;
in vec4 vColor;
out vec4 FragColor;

// Optional clip plane for portal half-body ghost rendering.
//   xyz = plane normal (world space)
//   w   = -dot(normal, point on plane)
// Pixel is discarded if dot(worldPos, xyz) + w < 0 — i.e. on the "wrong
// side" of the plane. Default vec4(0) = no clipping (xyz==0 short-
// circuits the test since the dot product is then zero and 0 + w < 0
// only when w < 0; callers pass (0,0,0,0) for "off").
uniform vec4 uClipPlane;

void main() {
    if (any(notEqual(uClipPlane.xyz, vec3(0.0))) &&
        dot(vWorldPos, uClipPlane.xyz) + uClipPlane.w < 0.0) {
        discard;
    }
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
            // The line's `u` carries the body's size (unused otherwise, 0):
            // a giant's limbs are as thick in proportion as anyone's.
            const float w = halfWidth * (va.u > 0.0f ? va.u : 1.0f);
            perp = glm::normalize(perp) * w;

            // Extend each endpoint along the line direction by the "miter
            // factor" — exactly halfWidth * tan(angleChange/2) — so adjacent
            // chained segments meet cleanly with no outer gap and no visible
            // diamond spike past the curve. StickFigureGeometry's 64-segment
            // head circle and 32-segment half-smile both share a 5.625° per-
            // segment angle change → tan(2.8125°) ≈ 0.049 is correct for both.
            constexpr float kMiterFactor = 0.049f;
            glm::vec3 along = glm::normalize(d) * (w * kMiterFactor);
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

        // Two vertex buffers per set (lines, triangles), two sets.
        for (FrameBuffers& fb : m_frames) {
            fb.lineVB = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, MAX_VERTICES * sizeof(StickVertex), nullptr, BufferAccess::Streaming);
            fb.lineMesh = g_renderBackend->CreateMesh(fb.lineVB, INVALID_BUFFER, GetBlockVertexLayout());

            fb.triVB = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, MAX_VERTICES * sizeof(StickVertex), nullptr, BufferAccess::Streaming);
            fb.triMesh = g_renderBackend->CreateMesh(fb.triVB, INVALID_BUFFER, GetBlockVertexLayout());
        }

        Log::Info("[PlayerRenderer] Initialized");
        return true;
    }

    void PlayerRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.lineMesh != INVALID_MESH) { g_renderBackend->DestroyMesh(fb.lineMesh); fb.lineMesh = INVALID_MESH; }
            if (fb.lineVB != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.lineVB); fb.lineVB = INVALID_BUFFER; }
            if (fb.triMesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(fb.triMesh);  fb.triMesh = INVALID_MESH; }
            if (fb.triVB != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(fb.triVB);  fb.triVB = INVALID_BUFFER; }
        }
        if (m_dummyTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_dummyTexture); m_dummyTexture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)       { g_renderBackend->DestroyShader(m_shader);       m_shader = INVALID_SHADER; }
    }

    // ------------------------------------------------------------------
    // Per-frame rendering
    // ------------------------------------------------------------------

    void PlayerRenderer::SubmitFigures(const glm::mat4& mvp, const glm::vec3& cameraPos,
                                       const glm::vec4& clipPlane) {
        // Which set, and where in it — see EntityFrame.hpp.
        if (m_frameCursor.Advance()) {
            m_lineCursor = 0;
            m_triCursor  = 0;
        }
        FrameBuffers& fb = m_frames[m_frameCursor.parity];

        // --- Pass 1: Triangles (head outline ring + back-of-head disc), all
        // back-face-culled. Ring is wound CCW from lookDir → visible from in
        // front of the player; disc is wound CCW from -lookDir → visible from
        // behind. CullMode::Back hides whichever side the camera isn't on.
        if (!m_triVerts.empty() && fb.triMesh != INVALID_MESH &&
            m_triCursor + m_triVerts.size() <= MAX_VERTICES) {
            g_renderBackend->UpdateBuffer(fb.triVB, m_triCursor * sizeof(StickVertex),
                m_triVerts.size() * sizeof(StickVertex), m_triVerts.data());

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
            g_renderBackend->SetUniformMat4(m_shader, "uMVP",   mvp);
            // Every caller bakes its transform into the vertices (see
            // RenderSingle), so the shader's own model matrix is identity
            // for BOTH passes.
            g_renderBackend->SetUniformMat4(m_shader, "uModel", glm::mat4(1.0f));
            g_renderBackend->SetUniformVec4(m_shader, "uClipPlane", clipPlane);
            g_renderBackend->DrawArrays(fb.triMesh, static_cast<uint32_t>(m_triVerts.size()),
                                        static_cast<uint32_t>(m_triCursor));
            g_renderBackend->UnbindMesh();
            m_triCursor += m_triVerts.size();
        }

        // --- Pass 2: Body/limbs/head outline/face features as camera-facing
        // thick triangle strips with FIXED WORLD-SPACE width. Replaces the old
        // PrimitiveType::Lines path so close players have visibly thick limbs
        // and far players naturally shrink via perspective (instead of the
        // GPU's always-1-logical-pixel rendering).
        if (!m_lineVerts.empty() && fb.lineMesh != INVALID_MESH) {
            // Width tuned so a player at ~5 m distance has limbs that read as
            // "stick-figure thick" (~1 px on a 1080p frame at default FOV).
            // Closer ⇒ thicker via perspective; farther ⇒ thinner.
            constexpr float kStripHalfWidth = 0.018f; // 1.8 cm half-width = 3.6 cm full

            m_stripVerts.clear();
            EmitThickWorldStripFromLines(m_lineVerts, cameraPos, kStripHalfWidth, m_stripVerts);

            if (!m_stripVerts.empty() && m_lineCursor + m_stripVerts.size() <= MAX_VERTICES) {
                g_renderBackend->UpdateBuffer(fb.lineVB, m_lineCursor * sizeof(StickVertex),
                    m_stripVerts.size() * sizeof(StickVertex), m_stripVerts.data());

                PipelineState stripState;
                stripState.depthTestEnabled  = true;
                stripState.depthWriteEnabled = true;
                stripState.blendEnabled      = false;
                stripState.cullMode          = CullMode::None;        // strips face camera; both sides visible
                stripState.primitiveType     = PrimitiveType::Triangles;
                g_renderBackend->SetPipelineState(stripState);

                g_renderBackend->BindShader(m_shader);
                g_renderBackend->BindTexture(m_dummyTexture, 0);
                g_renderBackend->SetUniformMat4(m_shader, "uMVP",   mvp);
                g_renderBackend->SetUniformMat4(m_shader, "uModel", glm::mat4(1.0f));
                g_renderBackend->SetUniformVec4(m_shader, "uClipPlane", clipPlane);
                g_renderBackend->DrawArrays(fb.lineMesh, static_cast<uint32_t>(m_stripVerts.size()),
                                            static_cast<uint32_t>(m_lineCursor));
                g_renderBackend->UnbindMesh();
                m_lineCursor += m_stripVerts.size();
            }
        }
    }

    void PlayerRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                const glm::vec3& cameraPos, const Frustum& frustum,
                                const Client::RemotePlayerManager& remotePlayers,
                                float partialTick,
                                const std::unordered_set<uint32_t>* skipIds,
                                const glm::vec4& clipPlane) {
        PROFILE_ZONE_N("PlayerRender");
        m_tally = Tally{};
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;

        const auto& players = remotePlayers.GetPlayers();
        if (players.empty()) return;

        // MC EntityDimensions for a standing player — the culling box. A
        // crouching player's 1.5 box sits inside this one, so the standing
        // box is the safe (never-pops) choice for both.
        constexpr float kPlayerWidth  = 0.6f;
        constexpr float kPlayerHeight = 1.8f;

        m_lineVerts.clear();
        m_triVerts.clear();   // ringTris + discTris combined; both back-face-culled
        m_lineVerts.reserve(players.size() * 36);
        // Ring: 64 segs * 6 verts = 384, smile: 32 * 6 = 192, disc: 16 * 3 = 48 → ~624/player.
        m_triVerts.reserve(players.size() * 640);

        for (const auto& [id, rp] : players) {
            if (!Client::IsRemotePlayerInBoundLevel(rp)) continue;
            if (skipIds && skipIds->count(id)) continue;
            ++m_tally.inLevel;
            // ── Sub-tick interpolation. Mirrors MC Entity.getPosition(partialTick)
            // (Entity.java:1955-1960), Entity.getYRot(partialTick) (:1918, uses
            // rotLerp for 360° wrap), Entity.getXRot(partialTick) (:1914, plain
            // lerp — pitch never wraps). Without this the renderer holds the
            // same value for ~3 frames per tick at 60fps then snaps, producing
            // visible 50ms-period stair-stepping.
            const glm::vec3 renderPos {
                glm::mix(rp.renderPrevPosition.x, rp.position.x, partialTick),
                glm::mix(rp.renderPrevPosition.y, rp.position.y, partialTick),
                glm::mix(rp.renderPrevPosition.z, rp.position.z, partialTick),
            };
            const float renderHeadYaw = Client::RotLerp(partialTick, rp.renderPrevRotation.x, rp.rotation.x);
            const float renderPitch   = glm::mix(           rp.renderPrevRotation.y, rp.rotation.y, partialTick);
            const float renderBodyYaw = Client::RotLerp(partialTick, rp.renderPrevBodyYaw,    rp.bodyYaw);

            // Distance-cull on the INTERPOLATED position so the cull boundary
            // matches what the user sees on screen (avoids edge-case cull pop
            // when prev/current straddle the 256m line).
            float dx = renderPos.x - cameraPos.x;
            float dz = renderPos.z - cameraPos.z;
            if (dx * dx + dz * dz > 256.0f * 256.0f) { ++m_tally.cullDistance; continue; }

            // MC Entity.shouldRenderAtSqrDistance for a 0.6x1.8 player: 64
            // blocks x viewScale (160 at a 20+ chunk view), before the Entity
            // Distance option.
            const float bodyWidth  = kPlayerWidth  * rp.scale;
            const float bodyHeight = kPlayerHeight * rp.scale;
            if (!EntityCulling::ShouldRenderAtSqrDistance(cameraPos, renderPos,
                                                          bodyWidth, bodyHeight)) {
                ++m_tally.cullDistance;
                continue;
            }

            // MC extractVisibleEntities: frustum AABB test, then the visible-
            // section gate. A player behind the camera used to be built and
            // uploaded every frame regardless.
            if (!EntityCulling::ShouldRender(frustum, renderPos, bodyWidth, bodyHeight)) {
                ++m_tally.cullFrustum;
                continue;
            }
            // The portal crossing passes draw only the players in a
            // surface (and the main pass, everyone else) — see
            // EntityCulling::g_crossingFilter.
            {
                const glm::vec3 half(bodyWidth * 0.5f, 0.0f, bodyWidth * 0.5f);
                if (!EntityCulling::PassesCrossingFilter(renderPos - half,
                                                         renderPos + half + glm::vec3(0.0f, bodyHeight, 0.0f))) {
                    ++m_tally.cullCrossing;
                    continue;
                }
            }
            ++m_tally.drawn;

            const auto& colorEntry = Game::LookupPlayerColor(rp.color);
            PlayerColor color{ colorEntry.r, colorEntry.g, colorEntry.b, 255 };

            // MC's hurt flash. The overlay texture's red row is 0xB2FF0000 and
            // the entity shader does `mix(overlay.rgb, color.rgb, overlay.a)`,
            // so the red contributes 1 - 178/255 = 0.302 — NOT the alpha
            // itself. Stick figures have no texture to overlay, so the same
            // blend is applied to the vertex colour and comes out identical.
            if (rp.hurtTime > 0) {
                constexpr float kMix = 178.0f / 255.0f;   // how much survives
                color.r = static_cast<uint8_t>(255.0f * (1.0f - kMix) + color.r * kMix);
                color.g = static_cast<uint8_t>(color.g * kMix);
                color.b = static_cast<uint8_t>(color.b * kMix);
            }
            // Append ring + disc into one shared list — both render with the
            // same triangles + CullMode::Back pipeline, so batching is fine.
            const size_t lineBegin = m_lineVerts.size();
            const size_t triBegin  = m_triVerts.size();
            BuildStickFigure(m_lineVerts, m_triVerts, m_triVerts, renderPos,
                             renderHeadYaw, renderBodyYaw, renderPitch, rp.isCrouching,
                             color);

            // The corpse falls over. Applied to just this player's slice of the
            // batch, which is why the two offsets above are taken first.
            const float flip = MobRenderer::DeathFlipDegrees(rp.deathTime, partialTick);
            ToppleStickFigure(m_lineVerts, lineBegin, renderPos, renderBodyYaw, flip);
            ToppleStickFigure(m_triVerts,  triBegin,  renderPos, renderBodyYaw, flip);
            ScaleStickFigure(m_lineVerts, lineBegin, renderPos, rp.scale);
            ScaleStickFigure(m_triVerts,  triBegin,  renderPos, rp.scale);
            for (size_t i = lineBegin; i < m_lineVerts.size(); ++i) m_lineVerts[i].u = rp.scale;
        }

        SubmitFigures(projection * view, cameraPos, clipPlane);

        // Restore default
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        defaultState.polygonMode       = PolygonMode::Fill;
        g_renderBackend->SetPipelineState(defaultState);
    }

    void PlayerRenderer::RenderSingle(const glm::mat4& projection, const glm::mat4& view,
                                      const glm::vec3& position,
                                      float headYaw, float bodyYaw, float pitch,
                                      bool isCrouching, uint8_t colorId,
                                      const glm::mat4& model,
                                      const glm::vec4& clipPlane,
                                      float deathFlipDeg) {
        PROFILE_ZONE_N("PlayerRenderSingle");
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;

        m_lineVerts.clear();
        m_triVerts.clear();

        const auto& colorEntry = Game::LookupPlayerColor(static_cast<Game::PlayerColorId>(colorId));
        PlayerColor color{ colorEntry.r, colorEntry.g, colorEntry.b, 255 };
        BuildStickFigure(m_lineVerts, m_triVerts, m_triVerts, position,
                         headYaw, bodyYaw, pitch, isCrouching, color);
        ToppleStickFigure(m_lineVerts, 0, position, bodyYaw, deathFlipDeg);
        ToppleStickFigure(m_triVerts,  0, position, bodyYaw, deathFlipDeg);

        // Pre-multiply the per-call `model` transform into the vertex
        // positions on the CPU. This is the portal pair matrix for ghost
        // renders (sends the player from src to dst side) and identity
        // for the normal path. Doing it CPU-side keeps uModel out of the
        // push-constant range — which matters on Vulkan where a mat4 won't
        // fit alongside uMVP (128-byte push-constant guarantee), and was
        // the cause of the ghost rendering at the ENTRY position with no
        // clipping in Vulkan ("can see my body when I look down at the
        // portal") because the Vulkan player shader silently ignored
        // uModel. After this transform `aPos` already IS the desired
        // world-space position, so the clip-plane test in the fragment
        // shader can use `vWorldPos = aPos` directly without uModel.
        //
        // SubmitFigures therefore passes identity as uModel for both passes.
        // (The strip pass used to pass `model` on top of the pre-multiplied
        // vertices — a double transform on GL, where the inline shader does
        // honour uModel.)
        if (model != glm::mat4(1.0f)) {
            // The model's size (a scaled body's BodyScaleModel) also sets
            // the limb thickness, through the line's `u`.
            const float modelScale = glm::length(glm::vec3(model[0]));
            for (auto& v : m_triVerts) {
                glm::vec4 w = model * glm::vec4(v.x, v.y, v.z, 1.0f);
                v.x = w.x; v.y = w.y; v.z = w.z;
            }
            for (auto& v : m_lineVerts) {
                glm::vec4 w = model * glm::vec4(v.x, v.y, v.z, 1.0f);
                v.x = w.x; v.y = w.y; v.z = w.z;
                v.u = modelScale;
            }
        }

        // For RenderSingle the camera position is recoverable from the
        // inverse view matrix's translation column — same convention as
        // the rest of the see-through pass uses.
        const glm::vec3 cameraPos = glm::vec3(glm::inverse(view)[3]);
        SubmitFigures(projection * view, cameraPos, clipPlane);
    }

    namespace {
        // Chat-bubble geometry batch. The old code created a Static VB/IB per
        // rect, drew, and destroyed them immediately: on Vulkan a Static
        // buffer with data goes through a staging copy that ends in
        // vkQueueWaitIdle (a full GPU drain, six or more per bubble per frame)
        // and the destroy raced the still-recorded draw. All rects now go
        // into one CPU batch, one Dynamic upload, one draw. Four slots rotate
        // per call: with two frames in flight and at most two calls a frame
        // (main pass + a portal re-entry), a slot is never rewritten while a
        // submitted frame can still read it.
        struct BubbleVertex { float x, y, z, u, v; uint8_t r, g, b, a; };
        static_assert(sizeof(BubbleVertex) == 24, "must match the block vertex layout");

        struct BubbleSlot {
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            size_t vbBytes = 0;
            size_t ibBytes = 0;
        };
        constexpr int kBubbleSlots = 4;
        BubbleSlot s_bubbleSlots[kBubbleSlots];
        int s_bubbleCursor = 0;
        std::vector<BubbleVertex> s_bubbleVerts;
        std::vector<uint32_t>     s_bubbleIndices;

        void AppendRect(int x0, int y0, int x1, int y1,
                        uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
            const uint32_t base = static_cast<uint32_t>(s_bubbleVerts.size());
            s_bubbleVerts.push_back({(float)x0, (float)y0, 0, 0, 0, cr, cg, cb, ca});
            s_bubbleVerts.push_back({(float)x1, (float)y0, 0, 0, 0, cr, cg, cb, ca});
            s_bubbleVerts.push_back({(float)x1, (float)y1, 0, 0, 0, cr, cg, cb, ca});
            s_bubbleVerts.push_back({(float)x0, (float)y1, 0, 0, 0, cr, cg, cb, ca});
            const uint32_t idx[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            s_bubbleIndices.insert(s_bubbleIndices.end(), idx, idx + 6);
        }

        // Uploads the batch into the next ring slot, growing it (1.5x, deferred
        // destroy of the old buffers) when the batch outgrew it. Returns null
        // if the backend refused the allocation.
        BubbleSlot* UploadBubbleGeometry() {
            BubbleSlot& slot = s_bubbleSlots[s_bubbleCursor];
            s_bubbleCursor = (s_bubbleCursor + 1) % kBubbleSlots;

            const size_t needVb = s_bubbleVerts.size() * sizeof(BubbleVertex);
            const size_t needIb = s_bubbleIndices.size() * sizeof(uint32_t);
            if (slot.vb == INVALID_BUFFER || slot.vbBytes < needVb || slot.ibBytes < needIb) {
                if (slot.mesh != INVALID_MESH) g_renderBackend->DeferredDestroyMesh(slot.mesh);
                if (slot.vb != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slot.vb);
                if (slot.ib != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slot.ib);
                slot.vbBytes = std::max(needVb + needVb / 2, sizeof(BubbleVertex) * 256);
                slot.ibBytes = std::max(needIb + needIb / 2, sizeof(uint32_t) * 384);
                slot.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, slot.vbBytes, nullptr,
                                                        BufferAccess::Dynamic);
                slot.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, slot.ibBytes, nullptr,
                                                        BufferAccess::Dynamic);
                slot.mesh = (slot.vb != INVALID_BUFFER && slot.ib != INVALID_BUFFER)
                    ? g_renderBackend->CreateMesh(slot.vb, slot.ib, GetBlockVertexLayout())
                    : INVALID_MESH;
                if (slot.mesh == INVALID_MESH) return nullptr;
            }
            g_renderBackend->UpdateBuffer(slot.vb, 0, needVb, s_bubbleVerts.data());
            g_renderBackend->UpdateBuffer(slot.ib, 0, needIb, s_bubbleIndices.data());
            return &slot;
        }
    } // namespace

    void PlayerRenderer::RenderChatBubbles(const glm::mat4& projection, const glm::mat4& view,
                                           const Client::RemotePlayerManager& remotePlayers,
                                           int fbWidth, int fbHeight) {
        if (!g_renderBackend || fbWidth <= 0 || fbHeight <= 0) return;

        glm::mat4 vp = projection * view;
        glm::mat4 ortho = glm::ortho(0.0f, static_cast<float>(fbWidth),
                                      static_cast<float>(fbHeight), 0.0f, -1.0f, 1.0f);

        // Pass 1: gather every visible bubble's rects into the CPU batch.
        s_bubbleVerts.clear();
        s_bubbleIndices.clear();
        for (const auto& [id, rp] : remotePlayers.GetPlayers()) {
            if (!Client::IsRemotePlayerInBoundLevel(rp)) continue;
            if (rp.chatBubbleTimer <= 0.0f || rp.chatBubbleText.empty()) continue;

            // Project player head position to screen
            glm::vec4 worldPos(rp.position.x, rp.position.y + 1.8f * rp.scale + 0.6f, rp.position.z, 1.0f);
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

            // Draw order within the batch is index order, so the outline is
            // appended first and the fill paints over it, as before.
            // Black outline
            AppendRect(bx-1, by-1, bx+bubbleW+1, by+bubbleH+1, 0,0,0,255);
            // White fill
            AppendRect(bx, by, bx+bubbleW, by+bubbleH, 255,255,255,255);
            // Triangle pointer
            int tx = static_cast<int>(screenX);
            int ty = by + bubbleH;
            AppendRect(tx-3, ty, tx+3, ty+1, 0,0,0,255);
            AppendRect(tx-2, ty+1, tx+2, ty+2, 0,0,0,255);
            AppendRect(tx-1, ty+2, tx+1, ty+3, 0,0,0,255);
            AppendRect(tx-2, ty, tx+2, ty+1, 255,255,255,255);
            AppendRect(tx-1, ty+1, tx+1, ty+2, 255,255,255,255);
        }
        if (s_bubbleIndices.empty()) return;

        // Pass 2: one upload, one draw.
        BubbleSlot* slot = UploadBubbleGeometry();
        if (!slot || slot->mesh == INVALID_MESH) return;

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
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", ortho);
        g_renderBackend->DrawIndexed(slot->mesh, static_cast<uint32_t>(s_bubbleIndices.size()));
        g_renderBackend->UnbindMesh();

        PipelineState def;
        def.depthTestEnabled = true;
        def.depthWriteEnabled = true;
        def.blendEnabled = false;
        def.cullMode = CullMode::Back;
        g_renderBackend->SetPipelineState(def);
    }

} // namespace Render
