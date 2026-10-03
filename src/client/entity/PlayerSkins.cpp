// File: src/client/entity/PlayerSkins.cpp
#include "client/entity/PlayerSkins.hpp"

#include "client/entity/RemotePlayerManager.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/world/fluid/FluidState.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Client {

    namespace {

        // Pixel art, sampled by exact texel: nearest, clamped (the cape and
        // skin sheets pack parts edge to edge).
        ::Render::TextureHandle Upload(const Game::SkinImage& image) {
            using namespace ::Render;
            if (!g_renderBackend || !image.Valid()) return INVALID_TEXTURE;
            const TextureHandle tex = g_renderBackend->CreateTexture2D(image.width, image.height,
                                                                       TextureFormat::RGBA8, image.rgba.data());
            if (tex == INVALID_TEXTURE) return INVALID_TEXTURE;
            g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            return tex;
        }

        ::Render::TextureHandle UploadSkin(const std::vector<uint8_t>& png) {
            Game::SkinImage image;
            if (!Game::DecodePng(png, image)) return ::Render::INVALID_TEXTURE;
            // MC SkinTextureDownloader.processLegacySkin: 64x32 grows to
            // 64x64, the base layer goes opaque.
            if (!Game::ProcessLegacySkin(image)) return ::Render::INVALID_TEXTURE;
            return Upload(image);
        }

        ::Render::TextureHandle UploadCape(const std::vector<uint8_t>& png) {
            Game::SkinImage image;
            if (!Game::DecodePng(png, image)) return ::Render::INVALID_TEXTURE;
            return Upload(image);
        }

        // MC Entity.isUnderWater / isInWater for a body the client only
        // knows the position of: water at the feet, and at the eye.
        void WaterAt(const glm::dvec3& feet, float eyeHeight, bool& inWater, bool& underWater) {
            inWater = underWater = false;
            if (!g_clientBlockAccess) return;
            const auto cellWater = [](const glm::dvec3& p) {
                const auto fluid = Game::GetFluidState(*g_clientBlockAccess,
                                                       static_cast<int>(std::floor(p.x)),
                                                       static_cast<int>(std::floor(p.y)),
                                                       static_cast<int>(std::floor(p.z)));
                return fluid.IsSame(Game::FluidType::Water);
            };
            inWater = cellWater(feet + glm::dvec3(0.0, 0.1, 0.0));
            underWater = cellWater(feet + glm::dvec3(0.0, static_cast<double>(eyeHeight), 0.0));
        }

    } // namespace

    // ── AvatarState ─────────────────────────────────────────────────────────

    void AvatarState::Tick(const glm::dvec3& pos, bool onGround, bool riding,
                           bool swimming, bool inWaterNow) {
        if (!initialized) {
            initialized = true;
            cloak = cloakO = pos;
            lastPos = pos;
        }
        const glm::dvec3 delta = pos - lastPos;
        lastPos = pos;

        // ClientAvatarState.tick: walkDistO, then moveCloak.
        walkDistO = walkDist;
        cloakO = cloak;
        const glm::dvec3 d = pos - cloak;
        constexpr double kTeleport = 10.0;
        if (d.x > kTeleport || d.x < -kTeleport) { cloak.x = pos.x; cloakO.x = cloak.x; } else { cloak.x += d.x * 0.25; }
        if (d.y > kTeleport || d.y < -kTeleport) { cloak.y = pos.y; cloakO.y = cloak.y; } else { cloak.y += d.y * 0.25; }
        if (d.z > kTeleport || d.z < -kTeleport) { cloak.z = pos.z; cloakO.z = cloak.z; } else { cloak.z += d.z * 0.25; }

        // Entity.move: walkDist grows by the horizontal travel × 0.6 (a body
        // that jumped across the world is not walking).
        const double horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        if (horizontal < kTeleport) walkDist += static_cast<float>(horizontal * 0.6);

        // AbstractClientPlayer.updateBob / rideTick's resetBob.
        bobO = bob;
        if (riding) {
            bob = 0.0f;
        } else {
            const float target = (onGround && !swimming) ? std::min(0.1f, static_cast<float>(horizontal)) : 0.0f;
            bob += (target - bob) * 0.4f;
        }

        // LivingEntity.updateSwimAmount.
        visuallySwimming = swimming;
        inWater = inWaterNow;
        swimAmountO = swimAmount;
        swimAmount = swimming ? std::min(1.0f, swimAmount + 0.09f) : std::max(0.0f, swimAmount - 0.09f);
    }

    CapePose ExtractCapePose(const AvatarState& state, float partialTick,
                             const glm::dvec3& prevPos, const glm::dvec3& pos,
                             float bodyYaw, float fallFlyingScale) {
        const double pt = static_cast<double>(partialTick);
        const glm::dvec3 cloak = state.cloakO + (state.cloak - state.cloakO) * pt;
        const glm::dvec3 body  = prevPos + (pos - prevPos) * pt;
        const double deltaX = cloak.x - body.x;
        const double deltaY = cloak.y - body.y;
        const double deltaZ = cloak.z - body.z;
        const double forwardX = std::sin(static_cast<double>(bodyYaw) * Game::Mth::kDegToRad);
        const double forwardZ = -std::cos(static_cast<double>(bodyYaw) * Game::Mth::kDegToRad);

        CapePose p;
        p.flap = std::clamp(static_cast<float>(deltaY) * 10.0f, -6.0f, 32.0f);
        p.lean = static_cast<float>(deltaX * forwardX + deltaZ * forwardZ) * 100.0f;
        p.lean *= 1.0f - fallFlyingScale;
        p.lean = std::clamp(p.lean, 0.0f, 150.0f);
        p.lean2 = static_cast<float>(deltaX * forwardZ - deltaZ * forwardX) * 100.0f;
        p.lean2 = std::clamp(p.lean2, -20.0f, 20.0f);
        const float bob = state.bobO + (state.bob - state.bobO) * partialTick;
        const float walk = state.walkDistO + (state.walkDist - state.walkDistO) * partialTick;
        p.flap += std::sin(walk * 6.0f) * 32.0f * bob;
        return p;
    }

    // ── PlayerSkins ─────────────────────────────────────────────────────────

    PlayerSkins& PlayerSkins::Get() {
        static PlayerSkins s_instance;
        return s_instance;
    }

    void PlayerSkins::Release(::Render::TextureHandle& texture) {
        if (texture == ::Render::INVALID_TEXTURE) return;
        // A frame in flight may still sample it.
        if (::Render::g_renderBackend) ::Render::g_renderBackend->DeferredDestroyTexture(texture);
        texture = ::Render::INVALID_TEXTURE;
    }

    void PlayerSkins::ReleaseEntry(Entry& entry) {
        Release(entry.skin);
        Release(entry.cape);
        entry.skinTried = false;
        entry.capeTried = false;
    }

    void PlayerSkins::SetLocal(const Game::PlayerAppearance& appearance) {
        if (appearance == m_local.appearance) return;
        ReleaseEntry(m_local);
        m_local.appearance = appearance;
        m_local.revision = m_nextRevision++;
        Log::Info("[PlayerSkins] local look: %s%s%s%s", Game::AppearanceModeSlug(appearance.mode),
                  appearance.IsSkin() ? (appearance.model == Game::SkinModel::Slim ? " slim" : " classic") : "",
                  appearance.IsSkin() && appearance.skinPng.empty() ? " (default skin)" : "",
                  appearance.HasCape() ? " + cape" : (appearance.hasPaint ? " (painted)" : ""));
        if (appearance.IsDrawn()) {
            Log::Info("[PlayerSkins] local figure is a drawing (%d strokes, %d points, %zu B)",
                      appearance.drawing.StrokeCount(), appearance.drawing.PointCount(),
                      appearance.drawing.EncodedSize());
        }
    }

    void PlayerSkins::SetRemote(uint32_t playerId, const Game::PlayerAppearance& appearance) {
        const auto [it, inserted] = m_remote.try_emplace(playerId);
        Entry& entry = it->second;
        if (!inserted && entry.appearance == appearance) return;
        ReleaseEntry(entry);
        entry.appearance = appearance;
        entry.revision = m_nextRevision++;
    }

    void PlayerSkins::Forget(uint32_t playerId) {
        const auto it = m_remote.find(playerId);
        if (it == m_remote.end()) return;
        ReleaseEntry(it->second);
        m_remote.erase(it);
    }

    void PlayerSkins::ClearRemote() {
        for (auto& [id, entry] : m_remote) ReleaseEntry(entry);
        m_remote.clear();
    }

    const Game::PlayerAppearance* PlayerSkins::Remote(uint32_t playerId) const {
        const auto it = m_remote.find(playerId);
        return it == m_remote.end() ? nullptr : &it->second.appearance;
    }

    bool PlayerSkins::IsSkinned(uint32_t playerId) const {
        const Game::PlayerAppearance* a = Remote(playerId);
        return a && a->IsSkin();
    }

    const Game::StickFigurePaint* PlayerSkins::RemotePaint(uint32_t playerId) const {
        const Game::PlayerAppearance* a = Remote(playerId);
        return (a && !a->IsSkin() && a->hasPaint) ? &a->paint : nullptr;
    }

    const Game::StickFigureDrawing* PlayerSkins::RemoteDrawing(uint32_t playerId, uint64_t* revision) const {
        const auto it = m_remote.find(playerId);
        if (it == m_remote.end() || !it->second.appearance.IsDrawn()) return nullptr;
        if (revision) *revision = it->second.revision;
        return &it->second.appearance.drawing;
    }

    const Game::StickFigureDrawing* PlayerSkins::LocalDrawing(uint64_t* revision) const {
        if (!m_local.appearance.IsDrawn()) return nullptr;
        if (revision) *revision = m_local.revision;
        return &m_local.appearance.drawing;
    }

    ::Render::TextureHandle PlayerSkins::DefaultSkin(Game::SkinModel model) {
        const int i = model == Game::SkinModel::Slim ? 1 : 0;
        if (m_defaultTried[i]) return m_defaultSkins[i];
        m_defaultTried[i] = true;
        std::vector<uint8_t> png;
        const std::string path = PlatformMain::GetAssetPath(Game::DefaultSkinAssetPath(model));
        if (!Game::ReadFileBytes(path, png, 1024 * 1024)) {
            Log::Warning("[PlayerSkins] missing default skin %s", path.c_str());
            return ::Render::INVALID_TEXTURE;
        }
        m_defaultSkins[i] = UploadSkin(png);
        return m_defaultSkins[i];
    }

    PlayerSkins::Textures PlayerSkins::Resolve(Entry& entry) {
        Textures t;
        const Game::PlayerAppearance& a = entry.appearance;
        t.slim = a.model == Game::SkinModel::Slim;
        t.modelParts = a.modelParts;
        if (!a.IsSkin()) return t;
        if (!a.skinPng.empty()) {
            if (!entry.skinTried) {
                entry.skinTried = true;
                entry.skin = UploadSkin(a.skinPng);
                if (entry.skin == ::Render::INVALID_TEXTURE) {
                    Log::Warning("[PlayerSkins] a skin failed to decode; drawing the default");
                }
            }
            t.skin = entry.skin;
        }
        // No skin of their own (or one that would not decode): MC
        // DefaultPlayerSkin for the chosen model.
        if (t.skin == ::Render::INVALID_TEXTURE) t.skin = DefaultSkin(a.model);
        if (a.HasCape() && (a.modelParts & Game::ModelPartBits::Cape)) {
            if (!entry.capeTried) {
                entry.capeTried = true;
                entry.cape = UploadCape(a.capePng);
            }
            t.cape = entry.cape;
        }
        return t;
    }

    PlayerSkins::Textures PlayerSkins::RemoteTextures(uint32_t playerId) {
        const auto it = m_remote.find(playerId);
        if (it == m_remote.end()) return Textures{};
        return Resolve(it->second);
    }

    PlayerSkins::Textures PlayerSkins::LocalTextures() {
        return Resolve(m_local);
    }

    void PlayerSkins::TickRemote(const RemotePlayerManager& players) {
        for (auto& [id, entry] : m_remote) {
            // The player model's cape and swim, and a drawn figure's swim tilt.
            if (!entry.appearance.IsSkin() && !entry.appearance.IsDrawn()) continue;
            const auto& all = players.GetPlayers();
            const auto it = all.find(id);
            if (it == all.end()) continue;
            const RemotePlayer& rp = it->second;
            if (!rp.positionInitialized) continue;
            // What the client can tell about a body it only sees move: on
            // the ground when it did not rise or fall this tick, swimming
            // when it sprints with its eye under water (MC updateSwimming:
            // sprinting, under water, not riding). Only the bound level's
            // blocks are known.
            const bool inLevel = IsRemotePlayerInBoundLevel(rp);
            bool inWater = false, underWater = false;
            if (inLevel) WaterAt(rp.position, 1.62f * rp.scale, inWater, underWater);
            const bool riding = rp.vehicleId != 0;
            const bool swimming = inLevel && rp.sprinting && underWater && !riding && !rp.fallFlying;
            const bool onGround = std::abs(rp.position.y - entry.physics.lastPos.y) < 1e-4 &&
                                  !rp.fallFlying && !inWater;
            entry.physics.Tick(rp.position, onGround, riding, swimming, inWater);
        }
    }

    void PlayerSkins::TickLocal(const glm::dvec3& pos, float headYaw, bool onGround, bool riding,
                                bool swimming, bool inWater) {
        const glm::dvec3 prev = m_local.physics.initialized ? m_local.physics.lastPos : pos;
        m_localPrevPos = prev;
        m_local.physics.Tick(pos, onGround, riding, swimming, inWater);

        // The rule the remote copies follow (RemotePlayerManager::Tick, MC
        // LivingEntity.aiStep + tickHeadTurn): moving, the body turns toward
        // the travel at 30 % a tick; the head may lead it by 50°.
        LocalBody& b = m_localBody;
        if (!b.initialized) {
            b.initialized = true;
            b.bodyYaw = b.bodyYawO = headYaw;
        }
        const glm::dvec3 d = pos - prev;
        b.bodyYawO = b.bodyYaw;
        const double speedSq = d.x * d.x + d.z * d.z;
        const float bodyTarget = speedSq > 0.0001 ? Game::Mth::YRotFromVector(glm::vec3(d)) : b.bodyYaw;
        b.bodyYaw += Game::Mth::WrapDegrees(bodyTarget - b.bodyYaw) * 0.3f;
        const float headOffset = Game::Mth::WrapDegrees(headYaw - b.bodyYaw);
        if (std::fabs(headOffset) > 50.0f) b.bodyYaw += headOffset - std::copysign(50.0f, headOffset);
        // MC LivingEntity.calculateEntityAnimation(false): horizontal travel
        // × 4, capped at 1, smoothed by 0.4 a tick (updateWalkAnimation); a
        // seat stops the limbs outright (walkAnimation.stop).
        if (riding) {
            b.walk.Stop();
        } else {
            b.walk.Update(std::min(1.0f, static_cast<float>(std::sqrt(speedSq)) * 4.0f), 0.4f, 1.0f);
        }
        ++b.ticks;
    }

    const AvatarState* PlayerSkins::RemoteAvatar(uint32_t playerId) const {
        const auto it = m_remote.find(playerId);
        return it == m_remote.end() ? nullptr : &it->second.physics;
    }

    void PlayerSkins::ReleaseTextures() {
        ReleaseEntry(m_local);
        for (auto& [id, entry] : m_remote) ReleaseEntry(entry);
        for (int i = 0; i < 2; ++i) {
            Release(m_defaultSkins[i]);
            m_defaultTried[i] = false;
        }
    }

} // namespace Client
