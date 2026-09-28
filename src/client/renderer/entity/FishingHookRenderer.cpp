// File: src/client/renderer/entity/FishingHookRenderer.cpp
//
// MC FishingHookRenderer — see the header. Reference:
// minecraft_code_26.3-pre-2/decompiled_net/minecraft/client/renderer/entity/
// FishingHookRenderer.java.
#include "client/renderer/entity/FishingHookRenderer.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/core/Frustum.hpp"
#include "client/renderer/core/RenderOrigin.hpp"
#include "client/renderer/entity/EntityCulling.hpp"
#include "client/renderer/entity/StickFigureGeometry.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "client/renderer/viewmodel/HeldItemRenderer.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/Morph.hpp"
#include "common/entity/projectile/FishingHook.hpp"

// Declaration only — STB_IMAGE_IMPLEMENTATION lives in one TU elsewhere.
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        // FishingHookRenderer.TEXTURE_LOCATION.
        constexpr const char* kHookTexturePath = "assets/textures/entity/fishing/fishing_hook.png";
        // FishingHook.shouldRenderAtSqrDistance: 64 blocks, flat.
        constexpr double kRenderDistanceSq = 4096.0;
        // The line: 16 steps (submit's `steps`).
        constexpr int kLineSteps = 16;
        // MC Camera's projection zNear, which getNearPlane scales by.
        constexpr double kZNear = 0.05;
        // FishingHookRenderer.VIEW_BOBBING_SCALE.
        constexpr double kViewBobbingScale = 960.0;

        // MC Player eye heights: standing 1.62, crouching 1.27.
        constexpr float kPlayerEye = 1.62f;
        constexpr float kCrouchEye = 1.27f;

        uint8_t ToByte(float v) {
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        struct LocalView {
            bool       set = false;
            bool       bodyVisible = false;
            glm::dvec3 eye{0.0};
            float      yawDeg = 0.0f;
            float      pitchDeg = 0.0f;
            float      fovDeg = 70.0f;
            int        fbWidth = 1920;
            int        fbHeight = 1080;
        };
        LocalView g_localView;

        // The stick figure's hand on the holding arm, scaled about the feet
        // as the renderer scales the figure (LeashRenderer's PlayerHandHold,
        // either arm).
        glm::dvec3 StickHand(const glm::dvec3& feet, float bodyYawDeg, bool crouching, bool sitting,
                             float scale, bool rightHand) {
            const glm::vec3 hand = StickFigureHand(glm::vec3(0.0f), bodyYawDeg, crouching, sitting, rightHand);
            return feet + glm::dvec3(hand) * static_cast<double>(scale);
        }

        // A morphed player's body is a mob's: 0.7 of its eye height, the
        // lead's Entity.getRopeHoldPosition.
        glm::dvec3 MorphHold(const glm::dvec3& feet, uint32_t morph, float scale) {
            const float eye = Game::Morph::IsValid(morph) ? Game::Morph::DimsOf(morph).eyeHeight : kPlayerEye;
            return feet + glm::dvec3(0.0, static_cast<double>(eye * scale) * 0.7, 0.0);
        }

        // MC Vec3.yRot / Vec3.xRot.
        glm::dvec3 VecYRot(const glm::dvec3& v, float angle) {
            const double c = std::cos(angle), s = std::sin(angle);
            return glm::dvec3(v.x * c + v.z * s, v.y, v.z * c - v.x * s);
        }
        glm::dvec3 VecXRot(const glm::dvec3& v, float angle) {
            const double c = std::cos(angle), s = std::sin(angle);
            return glm::dvec3(v.x, v.y * c + v.z * s, v.z * c - v.y * s);
        }

        uint32_t LocalPlayerId() {
            if (!Client::g_networkClient) return UINT32_MAX;
            auto connection = Client::g_networkClient->GetConnection();
            return connection ? connection->GetPlayerId() : UINT32_MAX;
        }

    } // namespace

    FishingHookRenderer::~FishingHookRenderer() = default;

    void FishingHookRenderer::SetLocalView(bool bodyVisible, const glm::dvec3& eye, float yawDeg,
                                           float pitchDeg, float fovDeg) {
        g_localView.set = true;
        g_localView.bodyVisible = bodyVisible;
        g_localView.eye = eye;
        g_localView.yawDeg = yawDeg;
        g_localView.pitchDeg = pitchDeg;
        g_localView.fovDeg = fovDeg > 1.0f ? fovDeg : 70.0f;
    }

    void FishingHookRenderer::SetFramebufferSize(int width, int height) {
        if (width > 0 && height > 0) {
            g_localView.fbWidth = width;
            g_localView.fbHeight = height;
        }
    }

    bool FishingHookRenderer::Initialize() {
        if (!g_renderBackend) return false;
        // The entity shader, as the leads use it: position, uv, colour; the
        // light is baked per vertex, fog is the frame's.
        m_shader = EntityEnvironment::CreateShader("shaders/entity.vert", "shaders/entity.frag");
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[FishingHookRenderer] failed to load the entity shader — bobbers will not render");
            return false;
        }
        for (FrameBuffers& fb : m_frames) {
            fb.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, kMaxVertices * sizeof(ModelVertex),
                                                  nullptr, BufferAccess::Streaming);
            fb.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, kMaxIndices * sizeof(uint32_t),
                                                  nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, fb.ib, GetBlockVertexLayout());
        }
        // The line is untextured (RenderTypes.lines: position, colour).
        const uint32_t whitePixel = 0xFFFFFFFFu;
        m_whiteTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, &whitePixel);
        m_verts.reserve(256);
        m_indices.reserve(512);
        m_initialized = true;
        return true;
    }

    void FishingHookRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(fb.mesh); fb.mesh = INVALID_MESH; }
            if (fb.vb != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.vb); fb.vb = INVALID_BUFFER; }
            if (fb.ib != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.ib); fb.ib = INVALID_BUFFER; }
        }
        if (m_whiteTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_whiteTexture); m_whiteTexture = INVALID_TEXTURE; }
        if (m_hookTexture != INVALID_TEXTURE)  { g_renderBackend->DestroyTexture(m_hookTexture);  m_hookTexture = INVALID_TEXTURE; }
        m_hookTextureTried = false;
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    TextureHandle FishingHookRenderer::HookTexture() {
        if (m_hookTextureTried) return m_hookTexture;
        m_hookTextureTried = true;
        const std::string full = PlatformMain::GetAssetPath(kHookTexturePath);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[FishingHookRenderer] missing texture %s", kHookTexturePath);
            return INVALID_TEXTURE;
        }
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) {
            Log::Warning("[FishingHookRenderer] failed to decode %s", kHookTexturePath);
            return INVALID_TEXTURE;
        }
        m_hookTexture = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);
        g_renderBackend->SetTextureFilter(m_hookTexture, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_hookTexture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        return m_hookTexture;
    }

    // ── MC getPlayerHandPos ────────────────────────────────────────────────

    bool FishingHookRenderer::ResolveHandPos(const Game::FishingHook& hook, float partialTick,
                                             glm::dvec3& out) const {
        const int32_t ownerId = hook.GetOwnerNetId();
        if (ownerId < 0 || ownerId >= Game::kItemEntityIdBase) return false;
        const double pt = static_cast<double>(partialTick);
        // getHoldingArm: the main arm (right) when the main hand holds the
        // rod, the other one otherwise.
        const bool rightArm = hook.GetOwnerHand() == 0;
        const int invert = rightArm ? 1 : -1;

        if (static_cast<uint32_t>(ownerId) == LocalPlayerId()) {
            // Only while the level being drawn is the player's own.
            if (Client::ClientLevels::BoundDimension() != Client::ClientLevels::ActiveDimension()) return false;
            const Game::ClientPlayer* player =
                Client::g_clientBlockAccess ? Client::g_clientBlockAccess->GetLocalPlayer() : nullptr;
            if (!player) return false;

            if (g_localView.set && !g_localView.bodyVisible) {
                // First person: the point on the camera's near plane where
                // the held rod's tip is, pushed out by 960 / fov and turned
                // by the arm swing.
                const float swing = g_heldItemRenderer.AttackAnim(partialTick);
                const float swing2 = std::sin(std::sqrt(swing) * 3.1415927f);
                const double fov = static_cast<double>(g_localView.fovDeg);
                const double aspect = static_cast<double>(g_localView.fbWidth) /
                                      static_cast<double>(std::max(1, g_localView.fbHeight));
                const double planeHeight = std::tan(fov * 0.017453292519943295 / 2.0) * kZNear;
                const double planeWidth = planeHeight * aspect;
                const double yaw = static_cast<double>(g_localView.yawDeg) * 0.017453292519943295;
                const double pitch = static_cast<double>(g_localView.pitchDeg) * 0.017453292519943295;
                // The camera's axes in MC's convention (yaw 0 looks +Z,
                // pitch positive looks down).
                const glm::dvec3 forward(-std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
                                         std::cos(yaw) * std::cos(pitch));
                const glm::dvec3 right(-std::cos(yaw), 0.0, -std::sin(yaw));
                const glm::dvec3 up = glm::cross(right, forward);
                // NearPlane.getPointOnPlane(x, y): forward + up * y - left * x.
                glm::dvec3 viewVec = forward * kZNear +
                                     up * (planeHeight * -0.1) +
                                     right * (planeWidth * static_cast<double>(invert) * 0.525);
                viewVec *= kViewBobbingScale / fov;
                viewVec = VecYRot(viewVec, swing2 * 0.5f);
                viewVec = VecXRot(viewVec, -swing2 * 0.7f);
                out = g_localView.eye + viewVec;
                return true;
            }

            const bool sitting = player->IsPassenger();
            const bool crouching = player->physics.isSneaking && !sitting;
            const float scale = player->physics.scale;
            const glm::dvec3 feet = player->visualPos;
            // The stick figure's body faces where the player looks.
            const float bodyYaw = g_localView.set ? g_localView.yawDeg : player->yaw;
            out = player->IsMorphed() ? MorphHold(feet, player->morph, scale)
                                      : StickHand(feet, bodyYaw, crouching, sitting, scale, rightArm);
            return true;
        }

        // A remote player in this level.
        if (!Client::g_remotePlayerManager) return false;
        const auto& players = Client::g_remotePlayerManager->GetPlayers();
        const auto it = players.find(static_cast<uint32_t>(ownerId));
        if (it == players.end()) return false;
        const Client::RemotePlayer& rp = it->second;
        if (!rp.positionInitialized || !Client::IsRemotePlayerInBoundLevel(rp)) return false;
        const glm::dvec3 feet = glm::mix(rp.renderPrevPosition, rp.position, pt);
        const float bodyYaw = Client::RotLerp(partialTick, rp.renderPrevBodyYaw, rp.bodyYaw);
        // PlayerRenderer's pose: seated on a cushion (a vehicle, not a bed)
        // never crouches.
        const bool sitting = rp.vehicleId != 0 && !rp.sleepingPos;
        out = rp.IsMorphed() ? MorphHold(feet, rp.morph, rp.scale)
                             : StickHand(feet, bodyYaw, rp.isCrouching && !sitting, sitting, rp.scale, rightArm);
        return true;
    }

    // ── Geometry ───────────────────────────────────────────────────────────

    void FishingHookRenderer::AppendBobber(const glm::dvec3& pos, const glm::vec3& right, const glm::vec3& up,
                                           const glm::vec3& light) {
        // submit: scale(0.5), rotate(camera.orientation), then the unit quad
        // (x - 0.5, y - 0.5) with u = x, v = 1 - y.
        const glm::vec3 base = ToRender(pos);
        const uint8_t r = ToByte(light.r), g = ToByte(light.g), b = ToByte(light.b);
        const uint32_t first = static_cast<uint32_t>(m_verts.size());
        const auto corner = [&](float x, float y, float u, float v) {
            const glm::vec3 p = base + right * ((x - 0.5f) * 0.5f) + up * ((y - 0.5f) * 0.5f);
            m_verts.push_back({p.x, p.y, p.z, u, v, r, g, b, 255});
        };
        corner(0.0f, 0.0f, 0.0f, 1.0f);
        corner(1.0f, 0.0f, 1.0f, 1.0f);
        corner(1.0f, 1.0f, 1.0f, 0.0f);
        corner(0.0f, 1.0f, 0.0f, 0.0f);
        const uint32_t idx[6] = {first, first + 1, first + 2, first, first + 2, first + 3};
        m_indices.insert(m_indices.end(), idx, idx + 6);
    }

    void FishingHookRenderer::AppendLine(const glm::dvec3& hookPos, const glm::dvec3& handPos,
                                         const glm::dvec3& camera, const glm::mat4& view,
                                         float pixelsPerUnitAtDepth1, float widthPx) {
        // extractRenderState: lineOriginOffset = hand - (hook + (0, 0.25, 0));
        // the line is drawn in the hook's pose.
        const glm::dvec3 origin = hookPos;
        const glm::dvec3 offset = handPos - (hookPos + glm::dvec3(0.0, 0.25, 0.0));
        const double xa = offset.x, ya = offset.y, za = offset.z;

        // stringVertex's curve and its tangent.
        const auto pointAt = [&](double a) {
            return origin + glm::dvec3(xa * a, ya * (a * a + a) * 0.5 + 0.25, za * a);
        };
        const auto tangentAt = [&](double a) {
            return glm::dvec3(xa, ya * (2.0 * a + 1.0) * 0.5, za);
        };

        const uint32_t first = static_cast<uint32_t>(m_verts.size());
        for (int i = 0; i <= kLineSteps; ++i) {
            const double a = static_cast<double>(i) / static_cast<double>(kLineSteps);
            const glm::dvec3 p = pointAt(a);
            const glm::vec3 rp = ToRender(p);
            // The ribbon's half-width for this vertex's view depth: widthPx on
            // screen whatever the distance (rendertype_lines' screen-space
            // expansion).
            const float depth = std::max(0.05f, -(view * glm::vec4(rp, 1.0f)).z);
            const float halfWidth = widthPx * 0.5f * depth / std::max(1.0e-3f, pixelsPerUnitAtDepth1);
            // Side-on to both the line and the eye.
            glm::dvec3 side = glm::cross(tangentAt(a), p - camera);
            const double len = glm::length(side);
            if (len > 1.0e-9) {
                side /= len;
            } else {
                side = glm::dvec3(view[0][0], view[1][0], view[2][0]);
            }
            const glm::vec3 s = glm::vec3(side) * halfWidth;
            // Black, opaque (setColor(-16777216)).
            m_verts.push_back({rp.x - s.x, rp.y - s.y, rp.z - s.z, 0.5f, 0.5f, 0, 0, 0, 255});
            m_verts.push_back({rp.x + s.x, rp.y + s.y, rp.z + s.z, 0.5f, 0.5f, 0, 0, 0, 255});
        }
        for (int i = 0; i < kLineSteps; ++i) {
            const uint32_t a = first + static_cast<uint32_t>(i) * 2;
            const uint32_t quad[6] = {a, a + 1, a + 3, a, a + 3, a + 2};
            m_indices.insert(m_indices.end(), quad, quad + 6);
        }
    }

    // ── The pass ───────────────────────────────────────────────────────────

    void FishingHookRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                                     const glm::vec3& cameraPos, const Frustum& frustum,
                                     const Client::ClientMobManager& mobs, float partialTick) {
        PROFILE_ZONE_N("FishingHookRender");
        (void)frustum;   // FishingHookRenderer.affectedByCulling: false
        if (!m_initialized || !g_renderBackend) return;

        if (Resources::CacheStale(m_textureGeneration)) {
            if (m_hookTexture != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(m_hookTexture);
            m_hookTexture = INVALID_TEXTURE;
            m_hookTextureTried = false;
        }

        if (m_frameCursor.Advance()) {
            m_vertCursor = 0;
            m_idxCursor  = 0;
        }
        FrameBuffers& fb = m_frames[m_frameCursor.parity];
        if (fb.mesh == INVALID_MESH) return;
        if (m_vertCursor >= kMaxVertices || m_idxCursor >= kMaxIndices) return;
        const size_t vertRoom = kMaxVertices - m_vertCursor;
        const size_t idxRoom  = kMaxIndices  - m_idxCursor;

        m_verts.clear();
        m_indices.clear();

        const glm::dvec3 camera(cameraPos);
        const double pt = static_cast<double>(partialTick);
        // The camera's own axes for the billboard (camera.orientation): the
        // view rotation's first two rows.
        const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 camUp(view[0][1], view[1][1], view[2][1]);
        // Window.getAppropriateLineWidth, and the pixels one unit spans at
        // view depth 1 (the projection's vertical scale over half the height).
        const float widthPx = std::max(2.5f, static_cast<float>(g_localView.fbWidth) / 1920.0f * 2.5f);
        const float pixelsPerUnitAtDepth1 = static_cast<float>(g_localView.fbHeight) * projection[1][1] * 0.5f;

        struct Pending { glm::dvec3 hook; glm::dvec3 hand; };
        std::vector<Pending> lines;

        for (const Client::ClientMob* entry : mobs.ModelMobList()) {
            if (!entry->mob || entry->mob->GetType() != Game::EntityTypeId::FishingBobber) continue;
            const auto& hook = static_cast<const Game::FishingHook&>(*entry->mob);
            if (hook.IsRemoved()) continue;
            const glm::dvec3 pos = glm::mix(entry->renderPrevPosition, hook.position, pt);
            {
                const glm::dvec3 d = pos - camera;
                if (glm::dot(d, d) >= kRenderDistanceSq) continue;
            }
            // shouldRender: && getPlayerOwner() != null.
            glm::dvec3 hand(0.0);
            if (!ResolveHandPos(hook, partialTick, hand)) continue;
            {
                const glm::vec3 lo = glm::vec3(glm::min(pos, hand)) - glm::vec3(0.5f);
                const glm::vec3 hi = glm::vec3(glm::max(pos, hand)) + glm::vec3(0.5f);
                if (!EntityCulling::PassesCrossingFilter(lo, hi)) continue;
            }
            // The light at the hook's probe (the eye, 0.2125 up).
            const glm::vec3 light = EntityEnvironment::LitAt(pos + glm::dvec3(0.0, hook.GetEyeHeight(), 0.0));
            AppendBobber(pos, camRight, camUp, light);
            lines.push_back({pos, hand});
            if (m_verts.size() + 64 > vertRoom || m_indices.size() + 128 > idxRoom) break;
        }
        const size_t bobberIndexCount = m_indices.size();
        for (const Pending& l : lines) {
            if (m_verts.size() + 64 > vertRoom || m_indices.size() + 128 > idxRoom) break;
            AppendLine(l.hook, l.hand, camera, view, pixelsPerUnitAtDepth1, widthPx);
        }

        if (m_indices.empty()) return;
        if (m_verts.size() > vertRoom || m_indices.size() > idxRoom) return;

        if (m_vertCursor > 0) {
            const auto base = static_cast<uint32_t>(m_vertCursor);
            for (uint32_t& i : m_indices) i += base;
        }
        g_renderBackend->UpdateBuffer(fb.vb, m_vertCursor * sizeof(ModelVertex),
                                      m_verts.size() * sizeof(ModelVertex), m_verts.data());
        g_renderBackend->UpdateBuffer(fb.ib, m_idxCursor * sizeof(uint32_t),
                                      m_indices.size() * sizeof(uint32_t), m_indices.data());
        const size_t firstIndex = m_idxCursor;
        m_vertCursor += m_verts.size();
        m_idxCursor  += m_indices.size();

        // Opaque cutout, unculled (the quad always faces the camera; the
        // ribbon has no back).
        PipelineState pipeline;
        pipeline.depthTestEnabled = true;
        pipeline.depthWriteEnabled = true;
        pipeline.blendEnabled = false;
        pipeline.cullMode = CullMode::None;
        pipeline.frontFace = FrontFace::CounterClockwise;
        pipeline.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(pipeline);

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * view);
        g_renderBackend->SetUniformVec4(m_shader, "uEntityClipPlane", ChunkRenderer::PortalEntityClipPlane());
        EntityEnvironment::ApplyWorld(m_shader, camera);
        g_renderBackend->SetUniformVec4(m_shader, "uColor", glm::vec4(0.0f));
        // The bobber's light is baked per vertex; the line has none.
        EntityEnvironment::SetEntityLight(m_shader, glm::vec3(EntityEnvironment::kEmissive));

        if (bobberIndexCount > 0) {
            const TextureHandle hookTex = HookTexture();
            if (hookTex != INVALID_TEXTURE) {
                g_renderBackend->BindTexture(hookTex, 0);
                g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(bobberIndexCount),
                                             static_cast<uint32_t>(firstIndex));
            }
        }
        const size_t lineIndexCount = m_indices.size() - bobberIndexCount;
        if (lineIndexCount > 0) {
            g_renderBackend->BindTexture(m_whiteTexture, 0);
            g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(lineIndexCount),
                                         static_cast<uint32_t>(firstIndex + bobberIndexCount));
        }
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
