// File: src/client/renderer/portal/ImmersivePortalRenderer.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "ImmersivePortalRenderer.hpp"

#include "../backend/RenderBackend.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../mesh/ClientMeshManager.hpp"
#include "../mesh/BlockHighlight.hpp"
#include "client/portal/ClientImmersivePortals.hpp"
#if ENABLE_PORTAL_GUN
#include "client/portal/ClientPortalManager.hpp"
#include <GLFW/glfw3.h>
#endif
#include "client/world/ClientLevel.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace Render {

    ImmersivePortalRenderer g_immersivePortalRenderer;

    namespace {

        using Game::Immersive::Portal;

        // Meshes of portals not seen for this many frames are freed.
        constexpr uint64_t kMeshEvictFrames = 600;
        // A portal is never frustum-culled when the eye is this close to
        // it: the test is unreliable with the camera in the surface.
        constexpr double kFrustumCullMinDistance = 0.1;
        // Surface vertex: pos3 + uv2 + rgba8, the block layout the portal
        // shaders were written for.
        struct SurfaceVertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(SurfaceVertex) == 24, "SurfaceVertex must match GetBlockVertexLayout stride");

        uint64_t ShapeKeyOf(const Portal& p) {
            // Enough to notice any change that alters the mesh.
            uint64_t h = 1469598103934665603ULL;
            auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
            mix(static_cast<uint64_t>(p.shape.type));
            mix(static_cast<uint64_t>(p.width * 1000.0));
            mix(static_cast<uint64_t>(p.height * 1000.0));
            mix(p.shape.vertices.size());
            mix(p.shape.indices.size());
            for (const auto& v : p.shape.vertices) {
                mix(static_cast<uint64_t>(static_cast<int64_t>(v.x * 1000.0f)));
                mix(static_cast<uint64_t>(static_cast<int64_t>(v.y * 1000.0f)));
            }
            for (uint32_t i : p.shape.indices) mix(i);
            return h;
        }

        // A projection that lands every vertex on the far plane: the clip
        // z row becomes the w row, so z/w == 1 (GL) and, after the Vulkan
        // backend's z' = 0.5z + 0.5w remap, still 1.
        glm::mat4 FarPlaneProjection(const glm::mat4& proj) {
            glm::mat4 far = proj;
            for (int c = 0; c < 4; ++c) far[c][2] = far[c][3];
            return far;
        }

        double DistanceToSurface(const Portal& p, const glm::dvec3& from) {
            glm::dvec3 mn, mx;
            p.BoundingBox(mn, mx, 0.0);
            const glm::dvec3 c = glm::clamp(from, mn, mx);
            return glm::length(c - from);
        }

    } // namespace

    // ── Lifecycle ──────────────────────────────────────────────────────────

    bool ImmersivePortalRenderer::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) {
            Log::Error("[ImmersivePortalRenderer] No render backend");
            return false;
        }
        // The gun's portal shader: a solid fill in uOutlineMode 0 with the
        // plain block vertex layout, already compiled for both backends.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal("shaders/portal.vert", "shaders/portal.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles("shaders/portal.vert", "shaders/portal.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[ImmersivePortalRenderer] Portal shader unavailable — portals will not render");
            return false;
        }
        unsigned char white[] = {255, 255, 255, 255};
        m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
        outlinesEnabled = std::getenv("OBEY_PORTAL_OUTLINES") != nullptr;
        m_initialized = true;
        Log::Info("[ImmersivePortalRenderer] initialised (max %d layers)", kMaxLayers);
        return true;
    }

    void ImmersivePortalRenderer::Shutdown() {
        if (g_renderBackend) {
            for (auto& [id, m] : m_meshes) DestroyMesh(m);
            if (m_dummyTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_dummyTexture); m_dummyTexture = INVALID_TEXTURE; }
            if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        }
        m_meshes.clear();
        m_initialized = false;
    }

    // ── Surface meshes ─────────────────────────────────────────────────────

    void ImmersivePortalRenderer::DestroyMesh(SurfaceMesh& m) {
        if (!g_renderBackend) return;
        if (m.mesh != INVALID_MESH) { g_renderBackend->DeferredDestroyMesh(m.mesh); m.mesh = INVALID_MESH; }
        if (m.vb != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(m.vb); m.vb = INVALID_BUFFER; }
        if (m.ib != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(m.ib); m.ib = INVALID_BUFFER; }
        m.indexCount = 0;
    }

    ImmersivePortalRenderer::SurfaceMesh& ImmersivePortalRenderer::MeshFor(const Portal& portal) {
        SurfaceMesh& m = m_meshes[portal.id];
        const uint64_t key = ShapeKeyOf(portal);
        m.lastUsedFrame = m_frame;
        if (m.mesh != INVALID_MESH && m.shapeKey == key) return m;
        DestroyMesh(m);

        // Local (u, v, 0): u along axisW, v along axisH — SurfaceModel maps
        // it onto the world. Rectangle = two triangles; Mesh = its own list.
        std::vector<SurfaceVertex> verts;
        std::vector<uint32_t> indices;
        const float hw = static_cast<float>(portal.HalfWidth());
        const float hh = static_cast<float>(portal.HalfHeight());
        if (portal.shape.IsRectangle()) {
            // UVs sit at the centre on purpose: the portal shader's solid
            // fill discards by radial distance from the UV centre (the gun's
            // oval), and a rectangle's corners would fall outside it.
            verts = {
                { -hw, -hh, 0.0f, 0.5f, 0.5f, 255, 255, 255, 255 },
                {  hw, -hh, 0.0f, 0.5f, 0.5f, 255, 255, 255, 255 },
                {  hw,  hh, 0.0f, 0.5f, 0.5f, 255, 255, 255, 255 },
                { -hw,  hh, 0.0f, 0.5f, 0.5f, 255, 255, 255, 255 },
            };
            indices = { 0, 1, 2, 0, 2, 3 };
        } else {
            verts.reserve(portal.shape.vertices.size());
            for (const auto& v : portal.shape.vertices) {
                verts.push_back({ v.x, v.y, 0.0f, 0.5f, 0.5f, 255, 255, 255, 255 });
            }
            indices = portal.shape.indices;
        }
        if (verts.empty() || indices.empty()) return m;

        m.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, verts.size() * sizeof(SurfaceVertex), verts.data());
        m.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, indices.size() * sizeof(uint32_t), indices.data());
        m.mesh = g_renderBackend->CreateMesh(m.vb, m.ib, GetBlockVertexLayout());
        m.indexCount = static_cast<uint32_t>(indices.size());
        m.shapeKey = key;
        return m;
    }

    void ImmersivePortalRenderer::EvictUnusedMeshes() {
        for (auto it = m_meshes.begin(); it != m_meshes.end();) {
            if (m_frame - it->second.lastUsedFrame > kMeshEvictFrames) {
                DestroyMesh(it->second);
                it = m_meshes.erase(it);
            } else {
                ++it;
            }
        }
    }

    namespace {
        // A gun portal opens over half a second: PortalRenderer's rim hole
        // grows as smoothstep(open)² of its full size. The surface follows
        // the same curve so the view and the rim expand together. The open
        // time lives on the gun pair; the record is matched by position.
        float GunOpenScale(const Portal& portal) {
#if ENABLE_PORTAL_GUN
            if (portal.kind != Game::Immersive::PortalKind::PortalGun) return 1.0f;
            const double now = glfwGetTime();
            float scale = 1.0f;
            bool found = false;
            Client::GetClientPortalManager().ForEachPair([&](uint64_t, const Client::ClientPortalPair& pair) {
                if (found) return;
                for (const Client::ClientPortal* cp : { &pair.blue, &pair.orange }) {
                    if (!cp->active || glm::length(cp->origin - portal.origin) > 0.05) continue;
                    const double open = glm::clamp((now - cp->openStartTimeSec) / Client::kOpenDurationSec, 0.0, 1.0);
                    const double smooth = open * open * (3.0 - 2.0 * open);
                    // ...and it breathes with the rim: PortalRenderer scales its
                    // mesh by 1 - 0.04·wobble at 0.35 Hz (its kPulseAmplitude /
                    // kPulseFreqHz, same clock), so the view must too or it
                    // shows past the rim at every trough.
                    const float t = static_cast<float>(std::fmod(now, 1000.0));
                    const float wobble = 0.5f + 0.5f * std::sin(t * 0.35f * 6.2831853f);
                    const float pulse  = 1.0f - 0.04f * wobble;
                    scale = static_cast<float>(std::max(smooth * smooth, 0.001)) * pulse;
                    found = true;
                    return;
                }
            });
            return scale;
#else
            (void)portal;
            return 1.0f;
#endif
        }
    }

    glm::mat4 ImmersivePortalRenderer::SurfaceModel(const Portal& portal) {
        // Drawn a few millimetres in front of the plane. A gun portal's
        // plane lies a hair off a wall face, and the mark pass depth-tests
        // against that face: at the same depth the surface and the stone
        // win alternate pixels as the camera moves. Render-only: the
        // crossing and the clip planes use the record's own plane.
        constexpr float kSurfaceBias = 0.0f;   // the mark pass carries its own margin now
        const glm::vec3 n(portal.Normal());
        const float open = GunOpenScale(portal);
        return glm::mat4{
            glm::vec4(glm::vec3(portal.axisW) * open, 0.0f),
            glm::vec4(glm::vec3(portal.axisH) * open, 0.0f),
            glm::vec4(n, 0.0f),
            glm::vec4(glm::vec3(portal.origin) + n * kSurfaceBias, 1.0f),
        };
    }

    PipelineState ImmersivePortalRenderer::BaseState() {
        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = false;
        s.colorWriteEnabled = false;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        // The camera may stand in the surface: without clamping, the part of
        // the quad in front of the near plane is discarded and the mask has
        // a hole exactly where the eye is about to pass through.
        s.depthClampEnabled = true;
        // Slope-scaled bias toward the viewer, as the gun's own mark pass
        // has: a gun portal's surface lies a hair off a wall face, and at a
        // grazing angle from a distance the two polygons' interpolated
        // depths cross over a constant offset — patches of stone win, and
        // the view (and the rim tested against the restored depth) vanish.
        s.depthBiasEnabled  = true;
        s.depthBiasSlope    = -2.0f;
        s.depthBiasConstant = -2.0f;
        s.stencilTestEnabled = true;
        s.stencilReadMask   = 0xFFu;
        s.stencilWriteMask  = 0xFFu;
        return s;
    }

    void ImmersivePortalRenderer::DrawSurface(const SurfaceMesh& mesh, const PipelineState& state,
                                              const glm::mat4& mvp, const glm::vec3& color) {
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_dummyTexture, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformVec3(m_shader, "uPortalColor", color);
        g_renderBackend->SetUniformFloat(m_shader, "uPulse", 1.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uForceFarDepth", 0.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uOutlineMode", 0.0f);   // solid fill
        g_renderBackend->SetUniformFloat(m_shader, "uTime", 0.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uTimeVS", 0.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uFlashIntensity", 0.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uOpenAmount", 1.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uOpenAmountVS", 1.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uStaticAmount", 0.0f);
        g_renderBackend->SetUniformFloat(m_shader, "uPortalActive", 1.0f);
        g_renderBackend->SetUniformInt(m_shader, "uUseTextures", 0);
        g_renderBackend->DrawIndexed(mesh.mesh, mesh.indexCount);
        g_renderBackend->UnbindMesh();
    }

    void ImmersivePortalRenderer::SetLayerOverride(int layer) {
        if (layer <= 0) {
            g_renderBackend->SetStencilOverride(false);
            return;
        }
        g_renderBackend->SetStencilOverride(true, CompareOp::Equal, StencilOp::Keep,
                                            static_cast<uint32_t>(layer), 0xFFu, 0u);
    }

    // ── Rendering ──────────────────────────────────────────────────────────

    void ImmersivePortalRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                         const Camera& camera, const Frustum& frustum,
                                         float aspect, float farPlane, float partialTick,
                                         const LevelRenderFn& renderLevel,
                                         const LevelRenderFn& renderCrossers) {
        if (!m_initialized || !g_renderBackend) return;
        ++m_frame;
        m_renderedThisFrame = 0;
        m_renderCrossers = &renderCrossers;

        if (Client::GetClientImmersivePortals().Count() > 0) {
            PROFILE_ZONE_N("ImmersivePortals");
            // Far entities poking into THIS world first, while its depth is
            // still the plain world's: they must occlude the surfaces marked
            // next, as anything in front of a portal does.
            RenderCrossers(0, nullptr, camera, view, projection, frustum, farPlane, partialTick);
            RenderLayer(0, nullptr, camera, view, projection, frustum, aspect, farPlane, partialTick, renderLevel);

            // Leave the pipeline where the HUD expects it.
            g_renderBackend->SetStencilOverride(false);
            g_renderBackend->SetCullInvert(false);
            ChunkRenderer::SetPortalClipPlane(glm::vec4(0.0f));
            EnvironmentState::Get().SetFrameOverride(nullptr);
            PipelineState defaultState;
            g_renderBackend->SetPipelineState(defaultState);
        }
        m_renderCrossers = nullptr;

        if (outlinesEnabled) {
            Client::GetClientImmersivePortals().ForEach([&](const Portal& p) {
                constexpr double kThickness = 0.02;
                const glm::dvec3 n = p.Normal();
                const glm::dvec3 w = p.axisW * p.width;
                const glm::dvec3 h = p.axisH * p.height;
                const glm::dvec3 d = n * kThickness;
                const glm::dvec3 corner = p.origin - w * 0.5 - h * 0.5 - d * 0.5;
                glm::mat4 model(1.0f);
                model[0] = glm::vec4(glm::vec3(w), 0.0f);
                model[1] = glm::vec4(glm::vec3(h), 0.0f);
                model[2] = glm::vec4(glm::vec3(d), 0.0f);
                model[3] = glm::vec4(glm::vec3(corner), 1.0f);
                g_blockHighlight.RenderOriented(model, projection, view);
            });
        }

        m_renderedLastFrame = m_renderedThisFrame;
        if ((m_frame % 60) == 0) EvictUnusedMeshes();
    }

    std::vector<ImmersivePortalRenderer::Candidate> ImmersivePortalRenderer::Candidates(
            int layer, const Portal* through, const glm::dvec3& eye, const Frustum& frustum,
            float farPlane) const {
        std::vector<Candidate> candidates;
        // The mod divides the range by the layer: deep reflections are not
        // drawn far away.
        const double range = static_cast<double>(farPlane) / static_cast<double>(std::max(1, layer + 1));
        Client::GetClientImmersivePortals().ForEach([&](const Portal& p) {
            if (!p.Has(Game::Immersive::PortalFlag::Visible)) return;
            // The portal we arrived through leads straight back to the
            // camera's own side: drawing it would show the outer world
            // inside itself (the mod's cannotRenderInMe).
            if (through && (p.id == through->reversePortalId || p.id == through->parallelPortalId)) return;
            if (!p.IsInFront(eye)) return;
            const double distance = DistanceToSurface(p, eye);
            if (distance > range) return;
            if (distance > kFrustumCullMinDistance) {
                glm::dvec3 mn, mx;
                p.BoundingBox(mn, mx, 0.05);
                if (!frustum.IsBoxVisible(glm::vec3(mn), glm::vec3(mx))) return;
            }
            candidates.push_back({ &p, distance });
        });
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        return candidates;
    }

    void ImmersivePortalRenderer::RenderCrossers(int layer, const Portal* through,
                                                 const Camera& camera, const glm::mat4& view,
                                                 const glm::mat4& projection, const Frustum& frustum,
                                                 float farPlane, float partialTick) {
        if (!m_renderCrossers) return;
        const glm::dvec3 eye(camera.position);
        const std::vector<Candidate> candidates = Candidates(layer, through, eye, frustum, farPlane);
        if (candidates.empty()) return;
        PROFILE_ZONE_N("ImmersivePortals.Crossers");

        const glm::vec4 outerClip = ChunkRenderer::PortalClipPlane();
        // These are cut at the exact surface: what the mark wipes of them
        // the far view draws back with its own margin.
        const float outerMargin = ChunkRenderer::PortalEntityClipMargin();
        ChunkRenderer::SetPortalEntityClipMargin(0.0f);
        for (const Candidate& c : candidates) {
            const Portal& portal = *c.portal;
            // Nothing physically exists behind a mirror's plane.
            if (portal.IsMirror()) continue;
            const Game::DimensionId farDim = portal.destDimension;

            // The far level drawn through the portal's INVERSE: far-space
            // positions land where they belong on this side.
            const glm::mat4 Minv(glm::inverse(portal.TransformMatrix()));
            Camera farCam = camera;
            farCam.position        = glm::vec3(portal.TransformPoint(eye));
            farCam.hasViewOverride = true;
            farCam.viewOverride    = view * Minv;
            farCam.viewTilt        = glm::mat4(1.0f);
            const glm::mat4 farView = farCam.GetViewMatrix();

            // Keep only what lies in FRONT of this surface: the near-space
            // outer plane, expressed in far space (a plane transforms by the
            // inverse transpose of the point transform: (M⁻¹)ᵀ p).
            const glm::vec4 nearPlane = portal.OuterClipPlane().AsClipPlane();
            ChunkRenderer::SetPortalClipPlane(glm::transpose(Minv) * nearPlane);

            glm::dvec3 corners[4];
            portal.Corners(corners);
            glm::vec3 farCorners[4];
            for (int i = 0; i < 4; ++i) farCorners[i] = glm::vec3(portal.TransformPoint(corners[i]));
            const Frustum baseFrustum = Frustum::FromMatrix(projection * farView);
            const Frustum farFrustum = c.distance < 0.35
                ? baseFrustum
                : Frustum::ThroughQuad(farCam.position, farCorners, baseFrustum);   // see RenderLayer

            Client::ClientLevels::WithLevel(farDim, [&]() {
                ViewContext ctx;
                ctx.dimension   = farDim;
                ctx.camera      = farCam;
                ctx.view        = farView;
                ctx.projection  = projection;
                ctx.frustum     = farFrustum;
                ctx.partialTick = partialTick;
                ctx.layer       = layer;
                ctx.through     = &portal;
                (*m_renderCrossers)(ctx);
            });
        }
        ChunkRenderer::SetPortalClipPlane(outerClip);
        ChunkRenderer::SetPortalEntityClipMargin(outerMargin);
    }

    void ImmersivePortalRenderer::RenderLayer(int layer, const Portal* through,
                                              const Camera& camera, const glm::mat4& view,
                                              const glm::mat4& projection, const Frustum& frustum,
                                              float aspect, float farPlane, float partialTick,
                                              const LevelRenderFn& renderLevel) {
        // ── Which portals of the BOUND level are worth drawing ──────────
        const glm::dvec3 eye(camera.position);
        const std::vector<Candidate> candidates = Candidates(layer, through, eye, frustum, farPlane);
        if (candidates.empty()) return;

        const int inner = layer + 1;
        const EnvironmentFrame* outerFrame = EnvironmentState::Get().FrameOverride();
        const glm::vec4 outerClip = ChunkRenderer::PortalClipPlane();
        const float outerMargin = ChunkRenderer::PortalEntityClipMargin();
        const bool outerCullInvert = g_renderBackend->CullInverted();

        for (const Candidate& c : candidates) {
            const Portal& portal = *c.portal;
            SurfaceMesh& mesh = MeshFor(portal);
            if (mesh.mesh == INVALID_MESH) continue;
            ++m_renderedThisFrame;

            const glm::mat4 model = SurfaceModel(portal);
            const glm::mat4 mvp   = projection * view * model;

            // Everything below draws the surface itself, so the enclosing
            // layer's override must not rewrite its stencil state.
            g_renderBackend->SetStencilOverride(false);

            // The mark's geometry (see 1.): the surface scaled about the eye
            // by `markShrink`, which puts its plane `markShift` in front of
            // the real one along the normal. The band between is wiped by
            // the mark and drawn back by the far view's entity passes — an
            // item lying in the surface showed a bare stripe there.
            const glm::vec3 eyeF(camera.position);
            // To the SURFACE, not its centre: a global surface's centre can
            // be a kilometre off while the eye is right at the plane.
            const float markDist   = std::max(0.2f, static_cast<float>(DistanceToSurface(portal, eye)));
            // The margin is the depth buffer's own resolution at that
            // distance with eight times the headroom: Δz ≈ z²/(near · 2²⁴)
            // for a 24-bit buffer, taken here at 2²¹. A flat 5 cm was safe at
            // any range but wiped every near-world face that runs into the
            // surface — the floor beside a frame — for 5 cm on either side
            // of the plane, where the far view has nothing: a thin line of
            // sky along the edges. Up close the margin is now half a
            // centimetre; far away it grows to what the buffer needs.
            const float nearPlane   = ChunkRenderer::NearPlane();
            const float kMarkMargin = std::clamp(markDist * markDist / (nearPlane * 2097152.0f), 0.005f, 0.25f);
            const float markShrink  = std::max(0.5f, 1.0f - kMarkMargin / markDist);
            const float markShift  =
                static_cast<float>(std::max(0.0, portal.SignedDistanceToPlane(eye))) * (1.0f - markShrink);

            // 1. Mark: EQUAL layer → layer+1 where the surface is visible.
            //
            //    "Visible" is decided the robust way round. Testing the
            //    surface LESS against the scene asks whether it beats the
            //    wall it is mounted on by a millimetre — a race that depth
            //    precision loses at some distance and angle, however it is
            //    biased. Instead the surface is drawn pushed kMarkMargin
            //    toward the viewer and tested LESS-OR-EQUAL against the scene:
            //    the mark lands wherever the scene is at or beyond that — the wall,
            //    the surface itself, open sky — and fails only where
            //    something stands clearly in front of the portal. The wall
            //    is then a full margin inside the accepted range at every
            //    distance; an occluder within the margin is painted over,
            //    which at five centimetres nobody sees. Source's portals
            //    punch their hole rather than race the wall for the same
            //    reason.
            {
                //    The push is toward the EYE, not along the normal: a scale
                //    about the eye moves every vertex closer along its own view
                //    ray, so the mark covers exactly the surface's screen
                //    outline (the rim's edge) while its depth gains the margin.
                //    A push along the normal would shift the outline by up to
                //    the margin in parallax, visibly past the rim up close.
                const glm::mat4 markModel =
                    glm::translate(glm::mat4(1.0f), eyeF) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(markShrink)) *
                    glm::translate(glm::mat4(1.0f), -eyeF) * model;
                PipelineState s = BaseState();
                s.depthCompareOp     = CompareOp::LessEqual;
                s.depthBiasEnabled   = false;   // the margin is the tolerance
                s.stencilCompareOp   = CompareOp::Equal;
                s.stencilReference   = static_cast<uint32_t>(layer);
                s.stencilPassOp      = StencilOp::IncrClamp;
                s.stencilFailOp      = StencilOp::Keep;
                s.stencilDepthFailOp = StencilOp::Keep;
                DrawSurface(mesh, s, projection * view * markModel, glm::vec3(0.0f));
            }

            // Far side: dimension, fog, camera.
            const Game::DimensionId farDim = portal.IsMirror() ? portal.dimension : portal.destDimension;
            const EnvironmentFrame farFrame = EnvironmentState::Get().FrameForDimension(farDim);

            // 2. Depth to far inside the mark, colour = the far fog (what the
            //    far sky pass leaves untouched must read as "sky").
            {
                PipelineState s = BaseState();
                s.depthCompareOp     = CompareOp::Always;
                s.depthWriteEnabled  = true;
                s.colorWriteEnabled  = true;
                s.stencilCompareOp   = CompareOp::Equal;
                s.stencilReference   = static_cast<uint32_t>(inner);
                s.stencilWriteMask   = 0u;
                DrawSurface(mesh, s, FarPlaneProjection(projection) * view * model, farFrame.fogColor);
            }

            // 3. The far world. Camera through the portal: position mapped,
            //    view = view · M⁻¹ (M: this side → far side), so the far
            //    world appears exactly where the surface is.
            {
                const glm::dmat4 M = portal.TransformMatrix();
                Camera farCam = camera;
                farCam.position        = glm::vec3(portal.TransformPoint(eye));
                farCam.hasViewOverride = true;
                farCam.viewOverride    = view * glm::mat4(glm::inverse(M));
                farCam.viewTilt        = glm::mat4(1.0f);   // already inside `view`
                const glm::mat4 farView = farCam.GetViewMatrix();

                // Only what is visible through the surface: a frustum whose
                // side planes pass through the eye and the surface's edges,
                // mapped to the far side.
                glm::dvec3 corners[4];
                portal.Corners(corners);
                glm::vec3 farCorners[4];
                for (int i = 0; i < 4; ++i) farCorners[i] = glm::vec3(portal.TransformPoint(corners[i]));
                // The portal-bounded frustum degenerates with the eye at the
                // surface (its side planes pass through the eye and the
                // edges; with no distance to speak of one flips and culls
                // half the far world). Within a third of a block the surface
                // fills the view anyway: use the plain camera frustum.
                const Frustum baseFrustum = Frustum::FromMatrix(projection * farView);
                const Frustum farFrustum = c.distance < 0.35
                    ? baseFrustum
                    : Frustum::ThroughQuad(farCam.position, farCorners, baseFrustum);

                // Nothing on the near side of the destination surface, cut
                // EXACTLY at it. The wall a flush portal sits on (the gun's,
                // the wand's) has its face on this plane, but that face
                // points away from the far camera and back-face culling
                // takes it; every face ON the plane that the camera can see
                // belongs to a block standing on the far surface and is
                // kept. A push into the far world was the seam: every far
                // face that ran into the plane stopped a centimetre short —
                // a line of sky along the edges, a stair you could see into.
                Game::Immersive::HalfSpace innerPlane = portal.InnerClipPlane();
                const glm::vec4 clip = innerPlane.AsClipPlane();
                ChunkRenderer::SetPortalClipPlane(clip);
                // A little past the mark's shift: with the two exactly
                // equal, rounding left a hairline at the seam between a
                // crossing body's two halves.
                constexpr float kEntityClipSlack = 0.02f;
                ChunkRenderer::SetPortalEntityClipMargin(markShift + kEntityClipSlack);
                SetLayerOverride(inner);
                // A reflection flips winding: front faces become back faces.
                const bool cullInvert = outerCullInvert != portal.IsMirror();
                g_renderBackend->SetCullInvert(cullInvert);
                EnvironmentState::Get().SetFrameOverride(&farFrame);

                Client::ClientLevels::WithLevel(farDim, [&]() {
                    ViewContext ctx;
                    ctx.dimension   = farDim;
                    ctx.camera      = farCam;
                    ctx.view        = farView;
                    ctx.projection  = projection;
                    ctx.frustum     = farFrustum;
                    ctx.partialTick = partialTick;
                    ctx.layer       = inner;
                    ctx.through     = &portal;
                    // Occlusion culling for this view starts just in front
                    // of the far surface, in air — never at farCam, which
                    // is usually inside the rock behind it.
                    if (g_chunkRenderer) {
                        // The far camera dropped onto the far surface, kept
                        // inside its outline, half a block into the far
                        // world: the point the view actually looks out of.
                        // The surface's centre is the wrong seed for a wide
                        // surface (a global one is kilometres across).
                        const glm::dvec3 farPoint = portal.IsMirror() ? portal.origin : portal.destination;
                        const glm::dvec3 content  = portal.ContentDirection();
                        const glm::dvec3 farW = glm::normalize(portal.TransformLocalVecNonScale(-portal.axisW));
                        const glm::dvec3 farH = glm::normalize(portal.TransformLocalVecNonScale(portal.axisH));
                        const glm::dvec3 rel  = glm::dvec3(farCam.position) - farPoint;
                        const double s    = portal.IsMirror() ? 1.0 : portal.scale;
                        const double u    = std::clamp(glm::dot(rel, farW), -portal.HalfWidth()  * s, portal.HalfWidth()  * s);
                        const double v    = std::clamp(glm::dot(rel, farH), -portal.HalfHeight() * s, portal.HalfHeight() * s);
                        const glm::dvec3 seed = farPoint + farW * u + farH * v + content * 0.5;
                        g_chunkRenderer->SetPortalViewSeed(glm::vec3(seed));
                    }
                    renderLevel(ctx);
                    if (g_chunkRenderer) g_chunkRenderer->ClearPortalViewSeed();

                    // Entities of the levels beyond THIS level's portals that
                    // poke into it — the same crossing pass as layer 0, one
                    // level deeper, under this view's stencil.
                    RenderCrossers(inner, &portal, farCam, farView, projection, farFrustum,
                                   farPlane, partialTick);

                    // Sections this view revealed as unmeshed get compiled:
                    // the far level's scheduler reads the visible set the
                    // chunk pass just recorded for it.
                    if (farDim != Client::ClientLevels::ActiveDimension()) {
                        ScheduleClientMeshBuilds(farCam.position);
                    }

                    if (inner < kMaxLayers) {
                        RenderLayer(inner, &portal, farCam, farView, projection, farFrustum,
                                    aspect, farPlane, partialTick, renderLevel);
                    }
                });

                EnvironmentState::Get().SetFrameOverride(outerFrame);
                g_renderBackend->SetCullInvert(outerCullInvert);
                ChunkRenderer::SetPortalClipPlane(outerClip);
                ChunkRenderer::SetPortalEntityClipMargin(outerMargin);
                g_renderBackend->SetStencilOverride(false);
            }

            // 4. Restore the surface's depth so this world's translucents and
            //    later portals occlude correctly against it.
            {
                PipelineState s = BaseState();
                s.depthCompareOp    = CompareOp::Always;
                s.depthWriteEnabled = true;
                s.stencilCompareOp  = CompareOp::Equal;
                s.stencilReference  = static_cast<uint32_t>(inner);
                s.stencilWriteMask  = 0u;
                DrawSurface(mesh, s, mvp, glm::vec3(0.0f));
            }

            // 5. Clamp: anything above `layer` (this portal and whatever was
            //    nested in it) goes back to `layer`.
            {
                PipelineState s = BaseState();
                s.depthTestEnabled  = false;
                s.stencilCompareOp  = CompareOp::Less;     // passes where layer < stencil
                s.stencilReference  = static_cast<uint32_t>(layer);
                s.stencilPassOp     = StencilOp::Replace;
                s.stencilFailOp     = StencilOp::Keep;
                s.stencilDepthFailOp = StencilOp::Keep;
                DrawSurface(mesh, s, mvp, glm::vec3(0.0f));
            }
        }

        // Back to the enclosing layer's mask for whatever it draws next.
        SetLayerOverride(layer);
    }

} // namespace Render

#endif // ENABLE_IMMERSIVE_PORTALS
