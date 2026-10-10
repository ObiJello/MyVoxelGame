// File: src/client/renderer/entity/PlayerRenderer.cpp
#include "PlayerRenderer.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/entity/PlayerSkins.hpp"
#include "common/world/block/BedBlock.hpp"
#include "StickFigureGeometry.hpp"
#include "EntityCulling.hpp"
#include "client/world/ClientLevel.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "MobRenderer.hpp"
#include "EntityOutline.hpp"
#include "../core/RenderOrigin.hpp"
#include "../environment/EntityEnvironment.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <iterator>

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

        // MC LivingEntityRenderer.setupRotations while fall-flying: the body
        // tipped forward about its own side axis by (90 + xRot), eased in
        // over the glide's first ten ticks (fallFlyTicks² / 100), so the
        // figure lies along where it looks.
        void GlideStickFigure(std::vector<StickVertex>& verts, size_t begin,
                              const glm::vec3& feet, float bodyYawDeg, float pitchDeg,
                              float fallFlyTicks) {
            if (fallFlyTicks <= 0.0f) return;
            const float ease = std::clamp(fallFlyTicks * fallFlyTicks / 100.0f, 0.0f, 1.0f);
            const float angle = ease * (90.0f + pitchDeg);
            if (angle == 0.0f) return;
            const glm::vec3 forward = Game::Mth::HorizontalViewVector(bodyYawDeg);
            // Up turned toward forward: about up × forward.
            const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward));
            const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(angle), axis);
            for (size_t i = begin; i < verts.size(); ++i) {
                const glm::vec3 p(verts[i].x, verts[i].y, verts[i].z);
                const glm::vec3 q = feet + glm::vec3(rot * glm::vec4(p - feet, 1.0f));
                verts[i].x = q.x;
                verts[i].y = q.y;
                verts[i].z = q.z;
            }
        }

        // MC LivingEntityRenderer.setupRotations while isAutoSpinAttack (a
        // riptide): the body laid along the look — rotateX(-90 - xRot) in
        // the body's frame — and spun about its own long axis,
        // rotateY(ageInTicks * -75). The spin comes first in vertex order
        // (it sits innermost on MC's pose stack), about world up; then the
        // tip, about the body's side axis (the glide's).
        void SpinStickFigure(std::vector<StickVertex>& verts, size_t begin,
                             const glm::vec3& feet, float bodyYawDeg, float pitchDeg,
                             float ageInTicks) {
            const glm::vec3 forward = Game::Mth::HorizontalViewVector(bodyYawDeg);
            const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward));
            const glm::mat4 tip  = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f + pitchDeg), side);
            const glm::mat4 spin = glm::rotate(glm::mat4(1.0f), glm::radians(ageInTicks * -75.0f),
                                               glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 rot = tip * spin;
            for (size_t i = begin; i < verts.size(); ++i) {
                const glm::vec3 p(verts[i].x, verts[i].y, verts[i].z);
                const glm::vec3 q = feet + glm::vec3(rot * glm::vec4(p - feet, 1.0f));
                verts[i].x = q.x;
                verts[i].y = q.y;
                verts[i].z = q.z;
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
    // The chat bubbles' shader (OpenGL 330 core; Vulkan loads
    // player_billboard_vk.*). The stick figures themselves draw with
    // shaders/stick_figure.* — lit and fogged.
    // Uses the block vertex layout: pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2).
    // UV is unused; color carries the colour.
    // ------------------------------------------------------------------



    // ------------------------------------------------------------------
    // Convert line pairs into camera-facing thick triangle strips with a
    // FIXED WORLD-SPACE width. Each line (a,b) becomes a quad whose two long
    // edges are (a,b) and the perpendicular `cross(b-a, camera-midpoint)`
    // gives the strip direction (always faces the camera). Width is in world
    // metres → perspective shrinks far players naturally; close players have
    // visibly thicker limbs.
    //
    // The line vertices are RENDER space (camera-relative), so `cameraPos`
    // must be the render-space eye too — a world position here would put
    // the strip direction off by the origin.
    // ------------------------------------------------------------------
    static void EmitThickWorldStripFromLines(const std::vector<StickVertex>& lineVerts,
                                             const glm::vec3& cameraPos,
                                             float halfWidth,
                                             std::vector<StickVertex>& triOut) {
        triOut.reserve(triOut.size() + (lineVerts.size() / 2) * 6);
        for (size_t i = 0; i + 1 < lineVerts.size(); i += 2) {
            const auto& va = lineVerts[i];
            const auto& vb = lineVerts[i + 1];
            // A drawn figure's stroke: the same strip at the stroke's own
            // width, with round joints and ends (StickFigureGeometry.hpp).
            if (IsDrawingStroke(va)) {
                AppendDrawingStrokeTriangles(va, vb, cameraPos, triOut);
                continue;
            }
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

        // The figures read the frame's fog (on Vulkan the portal pipeline
        // layout's Common UBO — EntityEnvironment::CreateShader).
        m_shader = EntityEnvironment::CreateShader("shaders/stick_figure.vert",
                                                   "shaders/stick_figure.frag");
        if (m_shader == INVALID_SHADER) {
            Log::Error("[PlayerRenderer] Failed to create the stick-figure shader");
            return false;
        }
        m_bubbleShader = g_renderBackend->CreateShaderFromFiles(
            "shaders/player_billboard.vert", "shaders/player_billboard.frag");
        if (m_bubbleShader == INVALID_SHADER) {
            Log::Error("[PlayerRenderer] Failed to create the chat-bubble shader");
            return false;
        }

        unsigned char white[] = {255, 255, 255, 255};
        m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);

        // Two vertex buffers per set (lines, triangles), two sets.
        for (int slot = 0; slot < EntityFrame::Slots(); ++slot) {
            FrameBuffers& fb = m_frames[slot];
            fb.lineVB = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, MAX_VERTICES * sizeof(StickVertex), nullptr, BufferAccess::Streaming);
            fb.lineMesh = g_renderBackend->CreateMesh(fb.lineVB, INVALID_BUFFER, GetBlockVertexLayout());

            fb.triVB = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, MAX_TRI_VERTICES * sizeof(StickVertex), nullptr, BufferAccess::Streaming);
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
        if (m_bubbleShader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_bubbleShader); m_bubbleShader = INVALID_SHADER; }
    }

    // ------------------------------------------------------------------
    // Per-frame rendering
    // ------------------------------------------------------------------

    namespace {
        // MC EntityRenderer.getPackedLightCoords for a player: the light at
        // the eye (Entity.getLightProbePosition), times the lightmap —
        // applied to the figure's colour after the hurt flash (entity.fsh
        // mixes the overlay in before the lightmap). All figures share one
        // draw, so each carries its own light in its vertex colour.
        // MC's hurt flash. The overlay texture's red row is 0xB2FF0000 and
        // the entity shader does `mix(overlay.rgb, color.rgb, overlay.a)`,
        // so the red contributes 1 - 178/255 = 0.302 — NOT the alpha
        // itself. Stick figures have no texture to overlay, so the same
        // blend is applied to the vertex colour and comes out identical.
        PlayerColor HurtFlash(PlayerColor color, bool hurt) {
            if (!hurt) return color;
            constexpr float kMix = 178.0f / 255.0f;   // how much survives
            color.r = static_cast<uint8_t>(255.0f * (1.0f - kMix) + color.r * kMix);
            color.g = static_cast<uint8_t>(color.g * kMix);
            color.b = static_cast<uint8_t>(color.b * kMix);
            return color;
        }

        PlayerColor LightFigureColor(PlayerColor c, const glm::dvec3& feetWorld,
                                     float scale, bool crouching) {
            const double eye = (crouching ? 1.27 : 1.62) * static_cast<double>(scale);
            const glm::vec3 light = EntityEnvironment::LitAt(feetWorld + glm::dvec3(0.0, eye, 0.0));
            auto mul = [](uint8_t v, float f) {
                return static_cast<uint8_t>(std::clamp(static_cast<float>(v) * f + 0.5f, 0.0f, 255.0f));
            };
            return PlayerColor{ mul(c.r, light.r), mul(c.g, light.g), mul(c.b, light.b), c.a };
        }
    }

    void PlayerRenderer::SubmitFigures(const glm::mat4& mvp, const glm::vec3& cameraPos,
                                       const glm::vec4& clipPlane, bool glowing, bool drawBody,
                                       bool translucent) {
        // Which set, and where in it — see EntityFrame.hpp.
        if (m_frameCursor.Advance()) {
            m_lineCursor = 0;
            m_triCursor  = 0;
        }
        FrameBuffers& fb = m_frames[m_frameCursor.slot];

        // --- Pass 1: Triangles (head outline ring + back-of-head disc), all
        // back-face-culled. Ring is wound CCW from lookDir → visible from in
        // front of the player; disc is wound CCW from -lookDir → visible from
        // behind. CullMode::Back hides whichever side the camera isn't on.
        // What fits of this call's triangles (whole triangles): a frame of
        // more figures than the set holds draws the first ones rather than none.
        const size_t triCount = m_triCursor < MAX_TRI_VERTICES
            ? std::min(m_triVerts.size(), (MAX_TRI_VERTICES - m_triCursor) / 3 * 3) : 0;
        if (triCount > 0 && fb.triMesh != INVALID_MESH) {
            // Unsynchronised: the ring is per frame slot and the cursor only
            // advances, so no draw of this frame or the one in flight reads
            // the range. The synchronised update made Apple's GL driver wait
            // for the previous draw of the buffer — 0.7 ms per body, 0.6 ms of
            // every frame with a portal in view (tour1, 2026-09-04).
            g_renderBackend->UpdateBufferUnsynchronized(fb.triVB, m_triCursor * sizeof(StickVertex),
                triCount * sizeof(StickVertex), m_triVerts.data());

            PipelineState triState;
            triState.depthTestEnabled  = true;
            triState.depthWriteEnabled = true;
            triState.blendEnabled      = translucent;   // MC itemEntityTranslucentCull
            triState.cullMode          = CullMode::Back;       // only show front-facing tris
            triState.frontFace         = FrontFace::CounterClockwise;
            triState.primitiveType     = PrimitiveType::Triangles;
            g_renderBackend->SetPipelineState(triState);

            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_dummyTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP",   mvp);
            // Every caller bakes its transform into the vertices (see
            // RenderSingle), so there is no model matrix for either pass.
            g_renderBackend->SetUniformVec4(m_shader, "uClipPlane", clipPlane);
            // The frame's fog, from this view's eye (render space, the
            // vertices' space). Each figure's light is baked into its vertex
            // colour (LightFigureColor), so the draw's own light is 1.
            EntityEnvironment::SetEntityLight(m_shader, glm::vec3(1.0f));
            EntityEnvironment::ApplyWorld(m_shader, Render::ToWorld(cameraPos));
            if (drawBody) {
                g_renderBackend->DrawArrays(fb.triMesh, static_cast<uint32_t>(triCount),
                                            static_cast<uint32_t>(m_triCursor));
                g_renderBackend->UnbindMesh();
            }
            if (glowing) {
                EntityOutline::Get().SubmitArrays(fb.triMesh, static_cast<uint32_t>(m_triCursor),
                                                  static_cast<uint32_t>(triCount),
                                                  m_dummyTexture, mvp);
            }
            m_triCursor += triCount;
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
            // What fits of this call's strips (whole triangles): a frame of
            // many detailed drawn figures draws the first ones rather than none.
            if (m_lineCursor < MAX_VERTICES) {
                m_stripVerts.resize(std::min(m_stripVerts.size(), (MAX_VERTICES - m_lineCursor) / 3 * 3));
            } else {
                m_stripVerts.clear();
            }

            if (!m_stripVerts.empty()) {
                g_renderBackend->UpdateBufferUnsynchronized(fb.lineVB, m_lineCursor * sizeof(StickVertex),
                    m_stripVerts.size() * sizeof(StickVertex), m_stripVerts.data());

                PipelineState stripState;
                stripState.depthTestEnabled  = true;
                stripState.depthWriteEnabled = true;
                stripState.blendEnabled      = translucent;
                stripState.cullMode          = CullMode::None;        // strips face camera; both sides visible
                stripState.primitiveType     = PrimitiveType::Triangles;
                g_renderBackend->SetPipelineState(stripState);

                g_renderBackend->BindShader(m_shader);
                g_renderBackend->BindTexture(m_dummyTexture, 0);
                g_renderBackend->SetUniformMat4(m_shader, "uMVP",   mvp);
                g_renderBackend->SetUniformVec4(m_shader, "uClipPlane", clipPlane);
                EntityEnvironment::SetEntityLight(m_shader, glm::vec3(1.0f));
                EntityEnvironment::ApplyWorld(m_shader, Render::ToWorld(cameraPos));
                if (drawBody) {
                    g_renderBackend->DrawArrays(fb.lineMesh, static_cast<uint32_t>(m_stripVerts.size()),
                                                static_cast<uint32_t>(m_lineCursor));
                    g_renderBackend->UnbindMesh();
                }
                if (glowing) {
                    EntityOutline::Get().SubmitArrays(fb.lineMesh, static_cast<uint32_t>(m_lineCursor),
                                                      static_cast<uint32_t>(m_stripVerts.size()),
                                                      m_dummyTexture, mvp);
                }
                m_lineCursor += m_stripVerts.size();
            }
        }
    }

    // ------------------------------------------------------------------
    // Drawn figures (Game::StickFigureDrawing, StickFigureGeometry.hpp)
    // ------------------------------------------------------------------

    const DrawingMesh* PlayerRenderer::DrawingMeshFor(uint32_t subjectId, bool headOnly) {
        const Client::PlayerSkins& skins = Client::PlayerSkins::Get();
        uint64_t revision = 0;
        const Game::StickFigureDrawing* drawing = subjectId == kLocalPlayer
            ? skins.LocalDrawing(&revision) : skins.RemoteDrawing(subjectId, &revision);
        if (!drawing) {
            m_drawingCache.erase(subjectId);
            return nullptr;
        }
        DrawingCacheEntry& entry = m_drawingCache[subjectId];
        if (entry.revision != revision) {
            PROFILE_ZONE_N("PlayerDrawingMesh");
            BuildDrawingMesh(*drawing, entry.mesh);
            entry.headBuilt = false;
            entry.revision = revision;
        }
        if (headOnly) {
            if (!entry.headBuilt) {
                BuildDrawingMesh(*drawing, entry.head, kDrawingHeadFrom);
                entry.headBuilt = true;
            }
            return &entry.head;
        }
        return &entry.mesh;
    }

    void PlayerRenderer::PruneDrawingCache() {
        const Client::PlayerSkins& skins = Client::PlayerSkins::Get();
        for (auto it = m_drawingCache.begin(); it != m_drawingCache.end();) {
            const bool keep = it->first == kLocalPlayer ? skins.LocalDrawing() != nullptr
                                                        : skins.RemoteDrawing(it->first) != nullptr;
            it = keep ? std::next(it) : m_drawingCache.erase(it);
        }
    }

    template <class Shade>
    size_t PlayerRenderer::AppendDrawing(std::vector<StickVertex>& out, const DrawingMesh& mesh,
                                         const glm::vec3& feet, float bodyYaw, bool crouching, Shade&& shade) {
        const size_t begin = out.size();
        // The palette through the figure's own light, hurt flash and
        // translucency — flat, like the limbs.
        PlayerColor palette[static_cast<int>(Game::PlayerColorId::Count)];
        for (int i = 0; i < static_cast<int>(Game::PlayerColorId::Count); ++i) {
            const auto& e = Game::LookupPlayerColor(static_cast<Game::PlayerColorId>(i));
            palette[i] = shade(PlayerColor{ e.r, e.g, e.b, 255 });
        }
        AppendDrawingLines(out, mesh, feet, bodyYaw, crouching, palette);
        return begin;
    }

    namespace {
        // A drawn figure in bed: MC lays the player model on its back
        // (setupRotations' sleeping branch), and a flat picture rolled on its
        // side like the stick figure would stand on its edge — so it is tipped
        // back about its own side axis, face up, the head toward where it
        // faced (the caller turns it to face the bed's foot).
        void LieDrawing(std::vector<StickVertex>& verts, size_t begin, const glm::vec3& feet, float bodyYawDeg) {
            const glm::vec3 forward = Game::Mth::HorizontalViewVector(bodyYawDeg);
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            // About the right: forward turns up, up turns back.
            const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), right);
            for (size_t i = begin; i < verts.size(); ++i) {
                const glm::vec3 q = feet + glm::vec3(rot * glm::vec4(verts[i].x - feet.x, verts[i].y - feet.y,
                                                                     verts[i].z - feet.z, 0.0f));
                verts[i].x = q.x;
                verts[i].y = q.y;
                verts[i].z = q.z;
            }
        }

        // MC AvatarRenderer.setupRotations' swim branch for a drawn figure:
        // rotateX(lerp(swimAmount, 0, inWater ? −90 − xRot : −90)) — the
        // glide's tip, about the same side axis — then, while visually
        // swimming, translate(0, −1, 0.3) in the tipped body's frame, which
        // puts the lying body over the swimmer's short box.
        void SwimDrawing(std::vector<StickVertex>& verts, size_t begin, const glm::vec3& feet,
                         float bodyYawDeg, float pitchDeg, float swimAmount, bool inWater,
                         bool visuallySwimming) {
            if (swimAmount <= 0.0f) return;
            const float angle = swimAmount * (inWater ? 90.0f + pitchDeg : 90.0f);
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            const glm::vec3 forward = Game::Mth::HorizontalViewVector(bodyYawDeg);
            const glm::vec3 axis = glm::normalize(glm::cross(up, forward));
            const glm::mat3 rot(glm::rotate(glm::mat4(1.0f), glm::radians(angle), axis));
            // The model frame's (0, −1, 0.3): down the body and toward its
            // back (MC's model +Z is behind the body), tipped with it.
            const glm::vec3 shift = visuallySwimming ? rot * (-up - 0.3f * forward) : glm::vec3(0.0f);
            for (size_t i = begin; i < verts.size(); ++i) {
                const glm::vec3 p(verts[i].x, verts[i].y, verts[i].z);
                const glm::vec3 q = feet + rot * (p - feet) + shift;
                verts[i].x = q.x;
                verts[i].y = q.y;
                verts[i].z = q.z;
            }
        }
    } // namespace

    void PlayerRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                const glm::vec3& cameraPos, const Frustum& frustum,
                                const Client::RemotePlayerManager& remotePlayers,
                                float partialTick,
                                const std::unordered_set<uint32_t>* skipIds,
                                const glm::vec4& clipPlane) {
        PROFILE_ZONE_N("PlayerRender");
        m_tally = Tally{};
        m_partialTick = partialTick;   // RenderSingle's too (a drawn figure's swim)
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;

        PruneDrawingCache();
        const auto& players = remotePlayers.GetPlayers();
        if (players.empty()) return;

        // MC EntityDimensions for a standing player — the culling box. A
        // crouching player's 1.5 box sits inside this one, so the standing
        // box is the safe (never-pops) choice for both.
        constexpr float kPlayerWidth  = 0.6f;
        constexpr float kPlayerHeight = 1.8f;

        m_lineVerts.clear();
        m_triVerts.clear();   // ringTris + discTris combined; both back-face-culled
        m_glowLineVerts.clear();
        m_glowTriVerts.clear();
        m_outlineOnlyLineVerts.clear();
        m_outlineOnlyTriVerts.clear();
        m_translucentLineVerts.clear();
        m_translucentTriVerts.clear();
        const bool collectingOutline = EntityOutline::Get().Collecting();
        m_lineVerts.reserve(players.size() * 36);
        // Ring: 64 segs * 6 verts = 384, smile: 32 * 6 = 192, disc: 16 * 3 = 48 → ~624/player.
        m_triVerts.reserve(players.size() * 640);

        for (const auto& [id, rp] : players) {
            if (!Client::IsRemotePlayerInBoundLevel(rp)) continue;
            if (rp.invisible) continue;   // /invisible
            if (rp.IsMorphed()) continue; // /morph: the mob renderer draws them
            // A Minecraft skin: the player model (MobRenderer::RenderPlayerSkins).
            if (Client::PlayerSkins::Get().IsSkinned(id)) continue;
            if (skipIds && skipIds->count(id)) continue;
            // MC LivingEntityRenderer.submit: an INVISIBLE body is not drawn
            // (the stick figure has no layers to keep) — unless it GLOWS,
            // when its outline alone is (the outline render type).
            // A spectator (drawn at all only for a spectator viewer — the
            // server marks them invisible to everyone else) is MC's
            // translucent floating head: PlayerModel shows only head and hat
            // when isSpectator, and the body is INVISIBLE (ServerPlayer
            // .updateInvisibilityStatus), which a spectator viewer sees
            // through LivingEntityRenderer's forceTransparent. A spectator
            // viewer sees any other INVISIBLE player the same way, whole.
            const bool spectatorHead = rp.IsSpectator();
            const bool glowing = collectingOutline && (rp.effects.Glowing() ||
                                                       (m_outlinePlayers && !spectatorHead));
            const bool bodyInvisible = rp.effects.Invisible();
            const bool translucent = spectatorHead || (bodyInvisible && m_viewerSeesInvisible);
            if (bodyInvisible && !glowing && !translucent) continue;
            // A drawn figure: its strokes replace the stick figure (the
            // spectator's floating head keeps them from the neck up).
            const DrawingMesh* drawing = DrawingMeshFor(id, spectatorHead);
            // Which batch the figure joins: drawn, drawn and outlined, or
            // outlined only — or the translucent batch (the head alone for a
            // spectator: the stick figure's lines are built and dropped, a
            // drawing's head strokes kept).
            std::vector<StickVertex>& lineOut = (spectatorHead && !drawing) ? m_scratchLineVerts
                                              : translucent ? m_translucentLineVerts
                                              : !glowing ? m_lineVerts
                                              : (bodyInvisible ? m_outlineOnlyLineVerts : m_glowLineVerts);
            std::vector<StickVertex>& triOut  = translucent ? m_translucentTriVerts
                                              : !glowing ? m_triVerts
                                              : (bodyInvisible ? m_outlineOnlyTriVerts : m_glowTriVerts);
            if (spectatorHead) m_scratchLineVerts.clear();
            ++m_tally.inLevel;
            // ── Sub-tick interpolation. Mirrors MC Entity.getPosition(partialTick)
            // (Entity.java:1955-1960), Entity.getYRot(partialTick) (:1918, uses
            // rotLerp for 360° wrap), Entity.getXRot(partialTick) (:1914, plain
            // lerp — pitch never wraps). Without this the renderer holds the
            // same value for ~3 frames per tick at 60fps then snaps, producing
            // visible 50ms-period stair-stepping.
            // In DOUBLE: the world position only becomes a float after the
            // render origin is subtracted (ToRender below). `cullPos` is the
            // float copy the world-space distance/frustum culls take — a few
            // centimetres loose far from the origin, which is all culling
            // needs.
            const glm::dvec3 renderPos =
                glm::mix(rp.renderPrevPosition, rp.position, static_cast<double>(partialTick));
            const glm::vec3 cullPos(renderPos);
            const float renderHeadYaw = Client::RotLerp(partialTick, rp.renderPrevRotation.x, rp.rotation.x);
            const float renderPitch   = glm::mix(           rp.renderPrevRotation.y, rp.rotation.y, partialTick);
            const float renderBodyYaw = Client::RotLerp(partialTick, rp.renderPrevBodyYaw,    rp.bodyYaw);

            // Distance-cull on the INTERPOLATED position so the cull boundary
            // matches what the user sees on screen (avoids edge-case cull pop
            // when prev/current straddle the 256m line).
            const double dx = renderPos.x - cameraPos.x;
            const double dz = renderPos.z - cameraPos.z;
            if (dx * dx + dz * dz > 256.0 * 256.0) { ++m_tally.cullDistance; continue; }

            // MC Entity.shouldRenderAtSqrDistance for a 0.6x1.8 player: 64
            // blocks x viewScale (160 at a 20+ chunk view), before the Entity
            // Distance option. A drawing may stand taller than the player
            // (up to 3 blocks): its box grows with it, so a tall hat in view
            // never pops while the body is off screen.
            const float bodyWidth  = kPlayerWidth  * rp.scale;
            const float bodyHeight = std::max(kPlayerHeight, drawing ? drawing->top : 0.0f) * rp.scale;
            if (!EntityCulling::ShouldRenderAtSqrDistance(cameraPos, cullPos,
                                                          bodyWidth, bodyHeight)) {
                ++m_tally.cullDistance;
                continue;
            }

            // MC extractVisibleEntities: frustum AABB test, then the visible-
            // section gate. A player behind the camera used to be built and
            // uploaded every frame regardless.
            if (!EntityCulling::ShouldRender(frustum, cullPos, bodyWidth, bodyHeight)) {
                ++m_tally.cullFrustum;
                continue;
            }
            // The portal crossing passes draw only the players in a
            // surface (and the main pass, everyone else) — see
            // EntityCulling::g_crossingFilter.
            {
                const glm::vec3 half(bodyWidth * 0.5f, 0.0f, bodyWidth * 0.5f);
                if (!EntityCulling::PassesCrossingFilter(cullPos - half,
                                                         cullPos + half + glm::vec3(0.0f, bodyHeight, 0.0f))) {
                    ++m_tally.cullCrossing;
                    continue;
                }
            }
            ++EntityCulling::g_renderedThisFrame;
            ++m_tally.drawn;

            const auto& colorEntry = Game::LookupPlayerColor(rp.color);
            const PlayerColor baseColor{ colorEntry.r, colorEntry.g, colorEntry.b, 255 };
            // GLOWING is MC's entity outline, not a tint (EntityOutline.hpp;
            // the glowing batches above).
            // Append ring + disc into one shared list — both render with the
            // same triangles + CullMode::Back pipeline, so batching is fine.
            //
            // The geometry is baked in RENDER space (camera-relative, see
            // RenderOrigin.hpp): the vertices go to the GPU as they are, so
            // the feet are moved next to the origin FIRST and the figure is
            // built, toppled and scaled about those small numbers. The
            // world-space `renderPos` above stays what the culls use.
            glm::vec3 renderFeet = Render::ToRender(renderPos);
            const size_t lineBegin = lineOut.size();
            const size_t triBegin  = triOut.size();

            // MC LivingEntityRenderer for Pose.SLEEPING: the figure lies
            // along the bed with its head toward the bed's facing, shifted
            // back from the head cell by (eyeHeight − 0.1) so the head rests
            // on the pillow end (submit's translate + setupRotations' 90°
            // flip). ToppleStickFigure rolls the body about its own forward
            // axis, so the head lands on the body's left; the body is built
            // facing 90° counter-clockwise of the bed so that side IS the
            // bed's head. Pitch and crouch are ignored in bed.
            float bodyYaw = renderBodyYaw, headYaw = renderHeadYaw, pitch = renderPitch;
            bool crouching = rp.isCrouching;
            float flip = MobRenderer::DeathFlipDegrees(rp.deathTime, partialTick);
            bool lyingDrawing = false;
            if (rp.sleepingPos && Client::g_clientBlockAccess) {
                const glm::ivec3 bed = *rp.sleepingPos;
                const Game::BlockState state =
                    Client::g_clientBlockAccess->GetBlockState(bed.x, bed.y, bed.z);
                if (Game::IsBedBlock(state.Block())) {
                    const Game::Direction facing = Game::BedFacing(state);
                    const glm::vec3 d(static_cast<float>(Game::StepX(facing)), 0.0f,
                                      static_cast<float>(Game::StepZ(facing)));
                    renderFeet -= d * (1.62f * rp.scale - 0.1f);
                    pitch = 0.0f;
                    crouching = false;
                    if (drawing) {
                        // A drawn figure lies on its back (LieDrawing): built
                        // facing the bed's foot, so tipping back puts the
                        // head on the pillow.
                        bodyYaw = headYaw = Game::ToYRot(Game::Opposite(facing));
                        lyingDrawing = true;
                    } else {
                        bodyYaw = headYaw = Game::ToYRot(Game::CounterClockWise(facing));
                        flip = 90.0f;
                    }
                }
            }

            // MC HumanoidModel's isPassenger pose while seated (a cushion).
            const bool sitting = rp.vehicleId != 0 && !rp.sleepingPos;
            if (sitting) crouching = false;
            // Every colour of the figure — its one colour, or each of a
            // painted figure's palette colours — through the hurt flash, the
            // light at the eye and the spectator's translucency.
            const auto shade = [&](PlayerColor c) {
                c = HurtFlash(c, rp.hurtTime > 0);
                c = LightFigureColor(c, renderPos, rp.scale, crouching);
                if (translucent) c.a = 38;   // ARGB 0x26FFFFFF, MC's forceTransparent tint
                return c;
            };
            // The drawn figure's strokes, or the stick figure. Every
            // transform below reaches either through this player's slice.
            if (drawing) {
                AppendDrawing(lineOut, *drawing, renderFeet, bodyYaw, crouching, shade);
            } else {
                const Game::StickFigurePaint* paint = Client::PlayerSkins::Get().RemotePaint(id);
                const StickFigureColors colors = paint ? StickFigureColors::FromPaint(*paint, shade)
                                                       : StickFigureColors::Uniform(shade(baseColor));
                BuildStickFigure(lineOut, triOut, triOut, renderFeet,
                                 headYaw, bodyYaw, pitch, crouching, colors, sitting);
            }

            // An elytra glide lays the figure along its flight — unless it
            // riptides, whose spin takes over (AvatarRenderer).
            if (rp.fallFlying && !rp.sleepingPos && !rp.autoSpinAttack) {
                const float ticks = static_cast<float>(rp.fallFlyTicks) + partialTick;
                GlideStickFigure(lineOut, lineBegin, renderFeet, bodyYaw, pitch, ticks);
                GlideStickFigure(triOut,  triBegin,  renderFeet, bodyYaw, pitch, ticks);
            }
            // A riptide spins the figure along its look.
            if (rp.autoSpinAttack && !rp.sleepingPos && rp.deathTime <= 0) {
                const float age = static_cast<float>(rp.tickCount) + partialTick;
                SpinStickFigure(lineOut, lineBegin, renderFeet, bodyYaw, pitch, age);
                SpinStickFigure(triOut,  triBegin,  renderFeet, bodyYaw, pitch, age);
            }
            // A drawn figure swims lying along its stroke (the player model's
            // tilt; the stick figure has none), and lies on its back in bed.
            if (drawing && !rp.fallFlying && !rp.autoSpinAttack && !rp.sleepingPos) {
                if (const Client::AvatarState* avatar = Client::PlayerSkins::Get().RemoteAvatar(id)) {
                    SwimDrawing(lineOut, lineBegin, renderFeet, bodyYaw, pitch, avatar->SwimAmount(partialTick),
                                avatar->inWater, avatar->visuallySwimming);
                }
            }
            if (lyingDrawing) LieDrawing(lineOut, lineBegin, renderFeet, bodyYaw);
            // The corpse falls over (or the sleeper lies down). Applied to
            // just this player's slice of the batch, which is why the two
            // offsets above are taken first.
            ToppleStickFigure(lineOut, lineBegin, renderFeet, bodyYaw, flip);
            ToppleStickFigure(triOut,  triBegin,  renderFeet, bodyYaw, flip);
            ScaleStickFigure(lineOut, lineBegin, renderFeet, rp.scale);
            ScaleStickFigure(triOut,  triBegin,  renderFeet, rp.scale);
            for (size_t i = lineBegin; i < lineOut.size(); ++i) ScaleLineWidth(lineOut[i], rp.scale);
        }

        // The strips are widened against the camera in the vertices' own
        // (render) space, so the camera goes in as a render-space point too.
        const glm::mat4 mvp = projection * view;
        const glm::vec3 renderEye = Render::ToRender(cameraPos);
        SubmitFigures(mvp, renderEye, clipPlane, /*glowing=*/false);
        // The GLOWING figures: drawn and outlined, then outlined only.
        if (!m_glowLineVerts.empty() || !m_glowTriVerts.empty()) {
            std::swap(m_lineVerts, m_glowLineVerts);
            std::swap(m_triVerts, m_glowTriVerts);
            SubmitFigures(mvp, renderEye, clipPlane, /*glowing=*/true);
            std::swap(m_lineVerts, m_glowLineVerts);
            std::swap(m_triVerts, m_glowTriVerts);
        }
        if (!m_outlineOnlyLineVerts.empty() || !m_outlineOnlyTriVerts.empty()) {
            std::swap(m_lineVerts, m_outlineOnlyLineVerts);
            std::swap(m_triVerts, m_outlineOnlyTriVerts);
            SubmitFigures(mvp, renderEye, clipPlane, /*glowing=*/true, /*drawBody=*/false);
            std::swap(m_lineVerts, m_outlineOnlyLineVerts);
            std::swap(m_triVerts, m_outlineOnlyTriVerts);
        }
        // The translucent figures (spectator heads, invisible bodies a
        // spectator sees), blended, after everything opaque.
        if (!m_translucentLineVerts.empty() || !m_translucentTriVerts.empty()) {
            std::swap(m_lineVerts, m_translucentLineVerts);
            std::swap(m_triVerts, m_translucentTriVerts);
            SubmitFigures(mvp, renderEye, clipPlane, /*glowing=*/false, /*drawBody=*/true,
                          /*translucent=*/true);
            std::swap(m_lineVerts, m_translucentLineVerts);
            std::swap(m_triVerts, m_translucentTriVerts);
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

    void PlayerRenderer::RenderSingle(const glm::mat4& projection, const glm::mat4& view,
                                      const glm::dvec3& position,
                                      float headYaw, float bodyYaw, float pitch,
                                      bool isCrouching, uint8_t colorId,
                                      const glm::dmat4& worldModel,
                                      const glm::dvec4& worldClipPlane,
                                      float deathFlipDeg,
                                      bool glowing,
                                      bool drawBody,
                                      bool isSitting,
                                      bool spectatorHead,
                                      float fallFlyTicks,
                                      float spinAttackAgeTicks,
                                      uint32_t subjectId) {
        PROFILE_ZONE_N("PlayerRenderSingle");
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        // The subject's look: a skin is the player model's to draw.
        const Client::PlayerSkins& skins = Client::PlayerSkins::Get();
        const bool local = subjectId == kLocalPlayer;
        if (local ? skins.LocalSkinned() : skins.IsSkinned(subjectId)) return;
        const Game::StickFigurePaint* paint = nullptr;
        if (local) {
            const Game::PlayerAppearance& own = skins.Local();
            if (own.hasPaint) paint = &own.paint;
        } else {
            paint = skins.RemotePaint(subjectId);
        }

        m_lineVerts.clear();
        m_triVerts.clear();

        // Render space from the start (RenderOrigin.hpp): the feet relative
        // to the view's origin, and the caller's WORLD model bridged into
        // that space in double — T(−R)·M·T(R) — so no vertex ever holds a
        // 300,000-block float. The `view` is render-space already
        // (Camera::GetViewMatrix), so uMVP = projection * view lands the
        // body where it belongs.
        const glm::dvec3 origin = Render::RenderOrigin();
        const glm::vec3 renderFeet = Render::ToRender(position);
        const glm::mat4 model(glm::translate(glm::dmat4(1.0), -origin) * worldModel *
                              glm::translate(glm::dmat4(1.0), origin));
        const glm::vec4 clipPlane = Render::PlaneToRender(worldClipPlane);

        const auto& colorEntry = Game::LookupPlayerColor(static_cast<Game::PlayerColorId>(colorId));
        const PlayerColor baseColor{ colorEntry.r, colorEntry.g, colorEntry.b, 255 };
        // The light where the body stands — the caller's `position`, not
        // where a portal model carries it (MC lights a ghost by its entity).
        if (isSitting) isCrouching = false;
        const auto shade = [&](PlayerColor c) {
            c = LightFigureColor(c, position, 1.0f, isCrouching);
            // A spectator's own body in third person: the translucent head
            // alone (PlayerModel with isSpectator, drawn with
            // forceTransparent — the viewer is a spectator too).
            if (spectatorHead) c.a = 38;
            return c;
        };
        // A drawn figure: its strokes instead of the stick figure (from the
        // neck up on a spectator's floating head); the glide, swim, spin,
        // topple and model below reach them through m_lineVerts.
        const DrawingMesh* drawing = DrawingMeshFor(subjectId, spectatorHead);
        if (drawing) {
            AppendDrawing(m_lineVerts, *drawing, renderFeet, bodyYaw, isCrouching, shade);
        } else {
            // The painted figure (the launcher's painter), if the subject has one.
            const StickFigureColors colors = paint ? StickFigureColors::FromPaint(*paint, shade)
                                                   : StickFigureColors::Uniform(shade(baseColor));
            BuildStickFigure(m_lineVerts, m_triVerts, m_triVerts, renderFeet,
                             headYaw, bodyYaw, pitch, isCrouching, colors, isSitting);
            if (spectatorHead) m_lineVerts.clear();
        }
        if (fallFlyTicks > 0.0f && spinAttackAgeTicks < 0.0f) {
            GlideStickFigure(m_lineVerts, 0, renderFeet, bodyYaw, pitch, fallFlyTicks);
            GlideStickFigure(m_triVerts,  0, renderFeet, bodyYaw, pitch, fallFlyTicks);
        }
        if (spinAttackAgeTicks >= 0.0f && deathFlipDeg == 0.0f) {
            SpinStickFigure(m_lineVerts, 0, renderFeet, bodyYaw, pitch, spinAttackAgeTicks);
            SpinStickFigure(m_triVerts,  0, renderFeet, bodyYaw, pitch, spinAttackAgeTicks);
        }
        // A drawn figure swims lying along its stroke (the player model's
        // tilt, from the subject's per-tick swim state).
        if (drawing && fallFlyTicks <= 0.0f && spinAttackAgeTicks < 0.0f && deathFlipDeg == 0.0f) {
            const Client::AvatarState* avatar = local ? &skins.LocalAvatar() : skins.RemoteAvatar(subjectId);
            if (avatar) {
                SwimDrawing(m_lineVerts, 0, renderFeet, bodyYaw, pitch, avatar->SwimAmount(m_partialTick),
                            avatar->inWater, avatar->visuallySwimming);
            }
        }
        ToppleStickFigure(m_lineVerts, 0, renderFeet, bodyYaw, deathFlipDeg);
        ToppleStickFigure(m_triVerts,  0, renderFeet, bodyYaw, deathFlipDeg);

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
                ScaleLineWidth(v, modelScale);
            }
        }

        // For RenderSingle the camera position is recoverable from the
        // inverse view matrix's translation column — same convention as
        // the rest of the see-through pass uses. `view` is render-space, so
        // the eye recovered here is the RENDER-space eye: exactly what the
        // strip widening wants now that the vertices are render-space too.
        const glm::vec3 cameraPos = glm::vec3(glm::inverse(view)[3]);
        const bool outline = glowing && EntityOutline::Get().Collecting();
        if (!drawBody && !outline) return;
        SubmitFigures(projection * view, cameraPos, clipPlane, outline, drawBody, spectatorHead);
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
            // /invisible or INVISIBILITY: a bubble would give the position away.
            if (rp.invisible || rp.effects.Invisible()) continue;
            if (rp.chatBubbleTimer <= 0.0f || rp.chatBubbleText.empty()) continue;

            // Project player head position to screen. `vp` is render-space
            // (the view is camera-relative), so the head goes through
            // ToRender before the multiply.
            const glm::dvec3 headWorld(rp.position.x, rp.position.y + 1.8 * rp.scale + 0.6, rp.position.z);
            glm::vec4 clip = vp * glm::vec4(Render::ToRender(headWorld), 1.0f);
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
        g_renderBackend->BindShader(m_bubbleShader);
        g_renderBackend->BindTexture(m_dummyTexture, 0);
        g_renderBackend->SetUniformMat4(m_bubbleShader, "uMVP", ortho);
        g_renderBackend->SetUniformVec4(m_bubbleShader, "uClipPlane", glm::vec4(0.0f));
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
