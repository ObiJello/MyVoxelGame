// File: src/client/renderer/entity/LeashRenderer.cpp
#include "client/renderer/entity/LeashRenderer.hpp"

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
#include "common/entity/Morph.hpp"
#include "common/entity/npc/Merchant.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/Leashable.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/decoration/LeashFenceKnot.hpp"
#include "common/world/lighting/LightCoords.hpp"

// Declaration only — STB_IMAGE_IMPLEMENTATION lives in one TU elsewhere.
#include "stb_image.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {

        namespace LC = Game::Lighting::LightCoords;

        // LeashFeatureRenderer.LEASH_RENDER_STEPS / LEASH_WIDTH.
        constexpr int   kLeashRenderSteps = 24;
        constexpr float kLeashWidth = 0.05f;

        // LeashKnotRenderer.KNOT_LOCATION.
        constexpr const char* kKnotTexturePath = "assets/textures/entity/lead_knot/lead_knot.png";
        // LeashFenceKnotEntity.shouldRenderAtSqrDistance: 32 blocks.
        constexpr double kKnotRenderDistanceSq = 1024.0;

        // MC Player eye heights: standing 1.62, crouching 1.27 (Player.POSES).
        constexpr float kPlayerEye = 1.62f;
        constexpr float kCrouchEye = 1.27f;

        uint8_t ToByte(float v) {
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        // MC EntityRenderer.getBlockLightLevel for the leashable and holding
        // mobs: burning ones, and the renderers that override it to 15 (of
        // the leashable mobs, the allay and the glow squid).
        bool FullBlockLight(const Game::Mob& mob) {
            if (mob.IsOnFire()) return true;
            switch (mob.GetType()) {
                case Game::EntityTypeId::Allay:
                case Game::EntityTypeId::GlowSquid:
                case Game::EntityTypeId::Blaze:
                case Game::EntityTypeId::MagmaCube:
                case Game::EntityTypeId::Vex:
                    return true;
                default:
                    return false;
            }
        }

        // DELIBERATE DIVERGENCE: a player holds the lead in the ENGINE'S
        // player model's hand, not at MC Player.getRopeHoldPosition's
        // offsets from MC's model. The stick figure's right hand
        // (StickFigureHand — the very joint its arm line ends at, with its
        // crouch and cushion-sitting poses), scaled about the feet as the
        // renderer scales the figure. The figure's arms have no swing or
        // walk animation, so neither does the rope's end.
        glm::dvec3 PlayerHandHold(const glm::dvec3& feet, float bodyYawDeg, bool crouching,
                                  bool sitting, float scale) {
            const glm::vec3 hand = StickFigureHand(glm::vec3(0.0f), bodyYawDeg, crouching, sitting,
                                                   /*rightHand=*/true);
            return feet + glm::dvec3(hand) * static_cast<double>(scale);
        }

        // A morphed player's body is a mob's: MC Entity.getRopeHoldPosition
        // (0.7 of the eye height) off that body.
        glm::dvec3 MorphRopeHold(const glm::dvec3& feet, uint32_t morph, float scale) {
            const float eye = Game::Morph::IsValid(morph) ? Game::Morph::DimsOf(morph).eyeHeight : kPlayerEye;
            return feet + glm::dvec3(0.0, static_cast<double>(eye * scale) * 0.7, 0.0);
        }

        // The local camera, published once per frame by the frame loop
        // (LeashRenderer::SetLocalView).
        struct LocalView {
            bool       set = false;
            bool       bodyVisible = false;   // third person / free camera
            glm::dvec3 eye{0.0};
            float      yawDeg = 0.0f;
            float      pitchDeg = 0.0f;
        };
        LocalView g_localView;

    } // namespace

    LeashRenderer::~LeashRenderer() = default;

    void LeashRenderer::SetLocalView(bool bodyVisible, const glm::dvec3& eye, float yawDeg, float pitchDeg) {
        g_localView.set = true;
        g_localView.bodyVisible = bodyVisible;
        g_localView.eye = eye;
        g_localView.yawDeg = yawDeg;
        g_localView.pitchDeg = pitchDeg;
    }

    bool LeashRenderer::Initialize() {
        if (!g_renderBackend) return false;
        // The entity shader (entity.vert/frag — the model family): position,
        // uv, colour; the draw's light rides uEntityLight, fog is the frame's.
        m_shader = EntityEnvironment::CreateShader("shaders/entity.vert", "shaders/entity.frag");
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[LeashRenderer] failed to load the entity shader — leads will not render");
            return false;
        }
        for (FrameBuffers& fb : m_frames) {
            fb.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, kMaxVertices * sizeof(ModelVertex),
                                                  nullptr, BufferAccess::Streaming);
            fb.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, kMaxIndices * sizeof(uint32_t),
                                                  nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, fb.ib, GetBlockVertexLayout());
        }
        // The rope is untextured (MC RenderTypes.leash: position, colour,
        // lightmap) — a 1x1 white stands in for the texture slot.
        const uint32_t whitePixel = 0xFFFFFFFFu;
        m_whiteTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, &whitePixel);

        // MC LeashKnotModel.createBodyLayer.
        m_knotModel = std::make_unique<ModelPart>();
        ModelPart* knot = m_knotModel->AddChild("knot", PartPose::Zero());
        CubeDefinition cube{};
        cube.originX = -3.0f; cube.originY = -8.0f; cube.originZ = -3.0f;
        cube.sizeX = 6.0f;    cube.sizeY = 8.0f;    cube.sizeZ = 6.0f;
        cube.texOffsX = 0.0f; cube.texOffsY = 0.0f;
        knot->cubes.push_back(cube);

        m_verts.reserve(4096);
        m_indices.reserve(8192);
        m_initialized = true;
        return true;
    }

    void LeashRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(fb.mesh); fb.mesh = INVALID_MESH; }
            if (fb.vb != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.vb); fb.vb = INVALID_BUFFER; }
            if (fb.ib != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.ib); fb.ib = INVALID_BUFFER; }
        }
        if (m_whiteTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_whiteTexture); m_whiteTexture = INVALID_TEXTURE; }
        if (m_knotTexture != INVALID_TEXTURE)  { g_renderBackend->DestroyTexture(m_knotTexture);  m_knotTexture = INVALID_TEXTURE; }
        m_knotTextureTried = false;
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_knotModel.reset();
        m_initialized = false;
    }

    TextureHandle LeashRenderer::KnotTexture() {
        if (m_knotTextureTried) return m_knotTexture;
        m_knotTextureTried = true;
        const std::string full = PlatformMain::GetAssetPath(kKnotTexturePath);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[LeashRenderer] missing texture %s", kKnotTexturePath);
            return INVALID_TEXTURE;
        }
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) {
            Log::Warning("[LeashRenderer] failed to decode %s", kKnotTexturePath);
            return INVALID_TEXTURE;
        }
        m_knotTexture = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);
        // Entity sheets: nearest, clamped (MobRenderer::LoadTexture's rule).
        g_renderBackend->SetTextureFilter(m_knotTexture, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_knotTexture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        return m_knotTexture;
    }

    // ── Holder resolution (MC Leashable.getLeashHolder's client branch) ────

    bool LeashRenderer::ResolveHolder(int32_t id, const Client::ClientMobManager& mobs,
                                      float partialTick, Holder& out) const {
        const double pt = static_cast<double>(partialTick);

        // A mob or a fence knot.
        if (Game::IsMobEntityId(id)) {
            const Client::ClientMob* entry = mobs.GetMob(id);
            if (!entry || !entry->mob || entry->mob->IsRemoved()) return false;
            const Game::Mob& holder = *entry->mob;
            out.position = glm::mix(entry->renderPrevPosition, holder.position, pt);
            out.bodyYawDeg = Game::Mth::RotLerp(partialTick, entry->renderPrevYBodyRot, holder.yBodyRot);
            out.eye = out.position + glm::dvec3(0.0, holder.GetEyeHeight(), 0.0);
            out.onFire = FullBlockLight(holder);
            if (const auto* knot = dynamic_cast<const Game::LeashFenceKnot*>(&holder)) {
                // LeashFenceKnotEntity.getRopeHoldPosition.
                out.ropeHold = knot->GetRopeHoldPosition();
            } else if (dynamic_cast<const Game::Merchant*>(&holder)) {
                // AbstractVillager.getRopeHoldPosition — a wandering trader
                // leading its llamas: (0, bbHeight - 1, 0.2) turned by the
                // body.
                const glm::dvec3 offset(0.0, static_cast<double>(holder.GetBbHeight()) - 1.0, 0.2);
                out.ropeHold = out.position + Game::Leash::YRot(offset, -out.bodyYawDeg * 0.017453292f);
            } else {
                // Entity.getRopeHoldPosition: 0.7 of the way up to the eye.
                out.ropeHold = out.position + glm::dvec3(0.0, static_cast<double>(holder.GetEyeHeight()) * 0.7, 0.0);
            }
            out.quadHolder = holder.SupportQuadLeashAsHolder();
            if (out.quadHolder) out.quadOffsets = holder.GetQuadLeashHolderOffsets();
            return true;
        }

        // A player: ids below the item range are connection ids.
        if (id < 0 || id >= Game::kItemEntityIdBase) return false;

        // The local player — only while the level being drawn is theirs.
        uint32_t localId = UINT32_MAX;
        if (Client::g_networkClient) {
            if (auto connection = Client::g_networkClient->GetConnection()) localId = connection->GetPlayerId();
        }
        if (static_cast<uint32_t>(id) == localId) {
            if (Client::ClientLevels::BoundDimension() != Client::ClientLevels::ActiveDimension()) return false;
            const Game::ClientPlayer* player =
                Client::g_clientBlockAccess ? Client::g_clientBlockAccess->GetLocalPlayer() : nullptr;
            if (!player) return false;
            const bool sitting = player->IsPassenger();
            const bool crouching = player->physics.isSneaking && !sitting;
            const float scale = player->physics.scale;
            out.position = player->visualPos;
            // The stick figure's body faces where the player looks.
            out.bodyYawDeg = g_localView.set ? g_localView.yawDeg : player->yaw;
            out.eye = out.position + glm::dvec3(0.0, (crouching ? kCrouchEye : kPlayerEye) * scale, 0.0);
            if (g_localView.set && !g_localView.bodyVisible) {
                // First person: MC LocalPlayer.getRopeHoldPosition — the lead
                // leaves the screen where the right hand is, (0.39 right,
                // 0.6 down, 0.3 ahead) of the eye, turned with the view.
                const float xRot = g_localView.pitchDeg * 0.017453292f;
                const float yRot = g_localView.yawDeg * 0.017453292f;
                const glm::dvec3 offset(0.39 * -1.0, -0.6, 0.3);   // mainArm RIGHT
                out.ropeHold = g_localView.eye +
                               Game::Leash::YRot(Game::Leash::XRot(offset, -xRot), -yRot);
            } else if (player->IsMorphed()) {
                out.ropeHold = MorphRopeHold(out.position, player->morph, scale);
            } else {
                out.ropeHold = PlayerHandHold(out.position, out.bodyYawDeg, crouching, sitting, scale);
            }
            out.onFire = false;
            out.quadHolder = false;
            return true;
        }

        // A remote player in this level.
        if (!Client::g_remotePlayerManager) return false;
        const auto& players = Client::g_remotePlayerManager->GetPlayers();
        const auto it = players.find(static_cast<uint32_t>(id));
        if (it == players.end()) return false;
        const Client::RemotePlayer& rp = it->second;
        if (!rp.positionInitialized || !Client::IsRemotePlayerInBoundLevel(rp)) return false;
        out.position = glm::mix(rp.renderPrevPosition, rp.position, pt);
        out.bodyYawDeg = Client::RotLerp(partialTick, rp.renderPrevBodyYaw, rp.bodyYaw);
        out.eye = out.position + glm::dvec3(0.0, (rp.isCrouching ? kCrouchEye : kPlayerEye) * rp.scale, 0.0);
        // PlayerRenderer's pose: seated on a cushion (a vehicle, not a bed)
        // never crouches.
        const bool sitting = rp.vehicleId != 0 && !rp.sleepingPos;
        out.ropeHold = rp.IsMorphed()
            ? MorphRopeHold(out.position, rp.morph, rp.scale)
            : PlayerHandHold(out.position, out.bodyYawDeg, rp.isCrouching && !sitting, sitting, rp.scale);
        out.onFire = false;
        out.quadHolder = false;
        return true;
    }

    // ── Geometry ───────────────────────────────────────────────────────────

    void LeashRenderer::AppendLeash(const LeashState& state) {
        // LeashFeatureRenderer.prepare.
        const float dx = static_cast<float>(state.end.x - state.start.x);
        const float dy = static_cast<float>(state.end.y - state.start.y);
        const float dz = static_cast<float>(state.end.z - state.start.z);
        const float horizontalSq = dx * dx + dz * dz;
        float dxOff, dzOff;
        if (horizontalSq > 1.0e-12f) {
            // Mth.invSqrt(dx² + dz²) * LEASH_WIDTH / 2: the ribbon's half
            // width, turned side-on to the rope.
            const float offsetFactor = 1.0f / std::sqrt(horizontalSq) * kLeashWidth / 2.0f;
            dxOff = dz * offsetFactor;
            dzOff = dx * offsetFactor;
        } else {
            // A rope hanging straight down: MC divides by zero here and draws
            // nothing; a fixed side-on width keeps it visible.
            dxOff = kLeashWidth / 2.0f;
            dzOff = 0.0f;
        }

        // Render space: the start in double relative to the origin, then the
        // rope's own small offsets in float (RenderOrigin.hpp).
        const glm::vec3 base = ToRender(state.start);
        const uint32_t first = static_cast<uint32_t>(m_verts.size());

        const auto addVertexPair = [&](float fudge, int k, bool backwards) {
            const float progress = static_cast<float>(k) / static_cast<float>(kLeashRenderSteps);
            const int block = static_cast<int>(Game::Mth::Lerp(progress, static_cast<float>(state.startBlockLight),
                                                               static_cast<float>(state.endBlockLight)));
            const int sky = static_cast<int>(Game::Mth::Lerp(progress, static_cast<float>(state.startSkyLight),
                                                             static_cast<float>(state.endSkyLight)));
            const glm::vec3 light = EntityEnvironment::LightColor(LC::Pack(block, sky));
            const float colorModifier = (k % 2 == (backwards ? 1 : 0)) ? 0.7f : 1.0f;
            const uint8_t r = ToByte(0.5f * colorModifier * light.r);
            const uint8_t g = ToByte(0.4f * colorModifier * light.g);
            const uint8_t b = ToByte(0.3f * colorModifier * light.b);
            const float x = dx * progress;
            float y;
            if (state.slack) {
                y = dy > 0.0f ? dy * progress * progress
                              : dy - dy * (1.0f - progress) * (1.0f - progress);
            } else {
                y = dy * progress;
            }
            const float z = dz * progress;
            m_verts.push_back({base.x + x - dxOff, base.y + y + fudge, base.z + z + dzOff,
                               0.5f, 0.5f, r, g, b, 255});
            m_verts.push_back({base.x + x + dxOff, base.y + y + kLeashWidth - fudge, base.z + z - dzOff,
                               0.5f, 0.5f, r, g, b, 255});
        };

        for (int k = 0; k <= kLeashRenderSteps; ++k) addVertexPair(kLeashWidth, k, false);
        for (int k = kLeashRenderSteps; k >= 0; --k) addVertexPair(0.0f, k, true);

        // RenderTypes.leash is a TRIANGLE_STRIP over the whole run (both
        // strips, joined end to end); as indexed triangles, every three
        // consecutive vertices. The rope draws unculled, so the strip's
        // alternating winding needs no flip.
        const uint32_t count = static_cast<uint32_t>(m_verts.size()) - first;
        for (uint32_t i = 0; i + 2 < count; ++i) {
            m_indices.push_back(first + i);
            m_indices.push_back(first + i + 1);
            m_indices.push_back(first + i + 2);
        }
    }

    void LeashRenderer::AppendKnot(const glm::dvec3& feet, const glm::vec3& light) {
        // LeashKnotRenderer.submit: at the entity's position, scale(-1, -1,
        // 1), the model (pixels → blocks: ModelPart's 1/16).
        const glm::vec3 origin = ToRender(feet);
        glm::mat4 m = glm::translate(glm::mat4(1.0f), origin);
        m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));
        const size_t first = m_verts.size();
        m_knotModel->Build(m, 32.0f, 32.0f, m_verts, m_indices, /*culled=*/false);
        // The cube's baked diffuse shade, times the knot cell's lightmap
        // colour (state.lightCoords) — per vertex, so every knot shares one
        // draw.
        for (size_t i = first; i < m_verts.size(); ++i) {
            ModelVertex& v = m_verts[i];
            v.r = ToByte(static_cast<float>(v.r) / 255.0f * light.r);
            v.g = ToByte(static_cast<float>(v.g) / 255.0f * light.g);
            v.b = ToByte(static_cast<float>(v.b) / 255.0f * light.b);
        }
    }

    // ── The pass ───────────────────────────────────────────────────────────

    void LeashRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                               const glm::vec3& cameraPos, const Frustum& frustum,
                               const Client::ClientMobManager& mobs, float partialTick) {
        PROFILE_ZONE_N("LeashRender");
        if (!m_initialized || !g_renderBackend) return;

        if (Resources::CacheStale(m_textureGeneration)) {
            // A resource-pack reload: the old sheet may still be read by the
            // frame in flight, so it goes through the deferred path.
            if (m_knotTexture != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(m_knotTexture);
            m_knotTexture = INVALID_TEXTURE;
            m_knotTextureTried = false;
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

        // ── Knots ───────────────────────────────────────────────────────
        for (const Client::ClientMob* entry : mobs.ModelMobList()) {
            if (!entry->mob || entry->mob->GetType() != Game::EntityTypeId::LeashKnot) continue;
            const Game::Mob& knot = *entry->mob;
            const glm::dvec3 d = knot.position - camera;
            // LeashFenceKnotEntity.shouldRenderAtSqrDistance — a flat 32
            // blocks, not the size-and-view-scale rule of other entities.
            if (glm::dot(d, d) >= kKnotRenderDistanceSq) continue;
            const glm::vec3 feet(knot.position);
            if (EntityCulling::g_crossingFilter) {
                const float hw = knot.GetBbWidth() * 0.5f;
                if (!EntityCulling::PassesCrossingFilter(feet - glm::vec3(hw, 0.0f, hw),
                                                         feet + glm::vec3(hw, knot.GetBbHeight(), hw))) {
                    continue;
                }
            }
            if (!EntityCulling::ShouldRender(frustum, feet, knot.GetBbWidth(), knot.GetBbHeight())) continue;
            // getLightProbePosition: the eye, a sixteenth above the feet.
            const glm::vec3 light = EntityEnvironment::LitAt(knot.position + glm::dvec3(0.0, knot.GetEyeHeight(), 0.0));
            AppendKnot(knot.position, light);
            if (m_verts.size() + 64 > vertRoom || m_indices.size() + 96 > idxRoom) break;
        }
        const size_t knotIndexCount = m_indices.size();

        // ── Ropes ───────────────────────────────────────────────────────
        std::array<LeashState, 4> states;
        for (const Client::ClientMob* entry : mobs.ModelMobList()) {
            if (!entry->mob) continue;
            const Game::Mob& mob = *entry->mob;
            const Game::Leash::LeashData* data = mob.GetLeashData();
            if (!data || data->delayedHolderId == Game::Leash::kNoHolder) continue;
            if (!mob.IsLeashable() || mob.IsRemoved()) continue;

            Holder holder;
            if (!ResolveHolder(data->delayedHolderId, mobs, partialTick, holder)) continue;

            // MC Entity.shouldRender: the leashed mob's own distance cutoff —
            // the rope is part of its render.
            const glm::dvec3 entityPos = glm::mix(entry->renderPrevPosition, mob.position, pt);
            {
                const glm::dvec3 d = entityPos - camera;
                if (!EntityCulling::ShouldRenderAtSqrDistance(glm::dot(d, d), mob.GetBbWidth(), mob.GetBbHeight())) {
                    continue;
                }
            }

            // EntityRenderer.extractRenderState's leash block.
            const float entityYRot = Game::Mth::RotLerp(partialTick, entry->renderPrevYBodyRot, mob.yBodyRot) *
                                     0.017453292f;
            const glm::dvec3 entityEye = entityPos + glm::dvec3(0.0, mob.GetEyeHeight(), 0.0);
            const int entityLight = EntityEnvironment::PackedLightAt(entityEye);
            const int holderLight = EntityEnvironment::PackedLightAt(holder.eye);
            const int startBlockLight = FullBlockLight(mob) ? 15 : LC::Block(entityLight);
            const int endBlockLight = holder.onFire ? 15 : LC::Block(holderLight);
            const int startSkyLight = LC::Sky(entityLight);
            const int endSkyLight = LC::Sky(holderLight);

            const bool quadConnection = holder.quadHolder && mob.SupportQuadLeash();
            const size_t leashCount = quadConnection ? 4 : 1;
            if (quadConnection) {
                const float roperYRot = holder.bodyYawDeg * 0.017453292f;
                const std::array<glm::dvec3, 4> leashableAttachmentPoints = mob.GetQuadLeashOffsets();
                for (size_t i = 0; i < 4; ++i) {
                    LeashState& s = states[i];
                    s.start = entityPos + Game::Leash::YRot(leashableAttachmentPoints[i], -entityYRot);
                    s.end = holder.position + Game::Leash::YRot(holder.quadOffsets[i], -roperYRot);
                    s.slack = false;
                }
            } else {
                LeashState& s = states[0];
                s.start = entityPos + Game::Leash::YRot(mob.GetLeashOffset(partialTick), -entityYRot);
                s.end = holder.ropeHold;
                // The single lead keeps LeashState's default: it sags.
                s.slack = true;
            }

            // MC EntityRenderer.shouldRender: drawn when the mob's box, the
            // holder's, or the space between is in view — the ropes' own box.
            glm::dvec3 lo = entityPos, hi = entityPos;
            for (size_t i = 0; i < leashCount; ++i) {
                lo = glm::min(lo, glm::min(states[i].start, states[i].end));
                hi = glm::max(hi, glm::max(states[i].start, states[i].end));
            }
            const glm::vec3 bmin = glm::vec3(lo) - glm::vec3(0.5f);
            const glm::vec3 bmax = glm::vec3(hi) + glm::vec3(0.5f);
            if (!EntityCulling::PassesCrossingFilter(bmin, bmax)) continue;
            if (frustum.TestAABB(bmin, bmax) == FrustumResult::Outside) continue;

            for (size_t i = 0; i < leashCount; ++i) {
                LeashState& s = states[i];
                s.startBlockLight = startBlockLight;
                s.endBlockLight = endBlockLight;
                s.startSkyLight = startSkyLight;
                s.endSkyLight = endSkyLight;
                AppendLeash(s);
            }
            if (m_verts.size() + 512 > vertRoom || m_indices.size() + 1536 > idxRoom) break;
        }

        if (m_indices.empty()) return;
        if (m_verts.size() > vertRoom || m_indices.size() > idxRoom) return;

        // Indices were built against this call's own list; rebase them onto
        // the set when an earlier call this frame filled its front.
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

        // Both render types draw unculled and opaque: RenderTypes.leash, and
        // the knot model's entityCutoutNoCull.
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
        // The light is baked per vertex (the rope's lerp, the knot's cell).
        EntityEnvironment::SetEntityLight(m_shader, glm::vec3(EntityEnvironment::kEmissive));

        if (knotIndexCount > 0) {
            const TextureHandle knotTex = KnotTexture();
            if (knotTex != INVALID_TEXTURE) {
                g_renderBackend->BindTexture(knotTex, 0);
                g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(knotIndexCount),
                                             static_cast<uint32_t>(firstIndex));
            }
        }
        const size_t ropeIndexCount = m_indices.size() - knotIndexCount;
        if (ropeIndexCount > 0) {
            g_renderBackend->BindTexture(m_whiteTexture, 0);
            g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(ropeIndexCount),
                                         static_cast<uint32_t>(firstIndex + knotIndexCount));
        }
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
