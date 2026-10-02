// File: src/client/entity/PlayerSkins.hpp
//
// The client's side of player appearance (docs/player-appearance.md): every
// player's look — the local player's from the launcher (--skin-mode, --skin,
// --skin-model, --cape, --stick-figure), everyone else's from
// PlayerAppearanceS2C — with the GPU textures their skins and capes draw
// with, and each body's cape physics and swim state.
//
//   * Textures upload lazily on first use, on the render thread, through the
//     render backend (nearest filtering, clamped: entity sheets are pixel
//     art). A 64x32 legacy skin is grown to 64x64 first (MC
//     SkinTextureDownloader.processLegacySkin). A replaced or forgotten
//     texture is released with DeferredDestroyTexture — a frame in flight
//     may still sample it (the Vulkan frame-overlap rule).
//   * Cape physics is MC ClientAvatarState: a lagging "cloak" point that
//     chases the body at a quarter of the gap per tick, the walk bob and the
//     walked distance — ticked once per client tick for every body that
//     wears a cape, read by the renderer as AvatarRenderer.extractCapeState's
//     capeFlap / capeLean / capeLean2.
//
// Client main thread only, except Local(), which is set once before the
// connection opens and read by the login path.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include "common/entity/LivingEntity.hpp"   // WalkAnimationState
#include "common/entity/PlayerAppearance.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>

namespace Client {

    class RemotePlayerManager;

    // MC ClientAvatarState (the cape's lagging anchor, the walk bob, the
    // walked distance) plus LivingEntity's swimAmount — the per-body state
    // the player model's renderer reads beyond the stick figure's, ticked
    // once per client tick.
    struct AvatarState {
        glm::dvec3 cloak{0.0}, cloakO{0.0};
        float bob = 0.0f, bobO = 0.0f;
        float walkDist = 0.0f, walkDistO = 0.0f;
        // MC LivingEntity.swimAmount / swimAmountO (updateSwimAmount: ±0.09
        // per tick toward isVisuallySwimming).
        float swimAmount = 0.0f, swimAmountO = 0.0f;
        bool  visuallySwimming = false;   // MC isVisuallySwimming (the SWIMMING pose)
        bool  inWater = false;            // MC isInWater (the swim tilt follows the look in water)
        glm::dvec3 lastPos{0.0};
        bool initialized = false;

        // One tick: `pos` is the body's position this tick, `onGround`
        // whether the bob may build (MC updateBob: on the ground, alive,
        // not swimming), `riding` resets it (rideTick).
        void Tick(const glm::dvec3& pos, bool onGround, bool riding,
                  bool swimming, bool inWaterNow);
        float SwimAmount(float partialTick) const {
            return swimAmountO + (swimAmount - swimAmountO) * partialTick;
        }
    };

    // AvatarRenderer.extractCapeState's output (degrees).
    struct CapePose {
        float flap = 0.0f;
        float lean = 0.0f;
        float lean2 = 0.0f;
    };
    // MC AvatarRenderer.extractCapeState from a body's state: `prevPos` /
    // `pos` the body's last two tick positions, `bodyYaw` the lerped body yaw
    // (degrees), `fallFlyingScale` AvatarRenderState.fallFlyingScale().
    CapePose ExtractCapePose(const AvatarState& state, float partialTick,
                             const glm::dvec3& prevPos, const glm::dvec3& pos,
                             float bodyYaw, float fallFlyingScale);

    class PlayerSkins {
    public:
        static PlayerSkins& Get();

        // ── The local player ────────────────────────────────────────────
        void SetLocal(const Game::PlayerAppearance& appearance);
        const Game::PlayerAppearance& Local() const { return m_local.appearance; }
        bool LocalSkinned() const { return m_local.appearance.IsSkin(); }

        // ── Everyone else (PlayerAppearanceS2C) ─────────────────────────
        void SetRemote(uint32_t playerId, const Game::PlayerAppearance& appearance);
        void Forget(uint32_t playerId);
        // A disconnect: every remote look and its textures.
        void ClearRemote();
        // Null for a player whose look never arrived (the plain stick figure).
        const Game::PlayerAppearance* Remote(uint32_t playerId) const;
        bool IsSkinned(uint32_t playerId) const;
        // The painted figure a remote player chose, or null.
        const Game::StickFigurePaint* RemotePaint(uint32_t playerId) const;

        // ── Textures (render thread, backend up) ────────────────────────
        struct Textures {
            ::Render::TextureHandle skin = ::Render::INVALID_TEXTURE;   // 64x64
            ::Render::TextureHandle cape = ::Render::INVALID_TEXTURE;   // 64x32, or none
            bool slim = false;
            uint8_t modelParts = Game::ModelPartBits::All;
        };
        Textures RemoteTextures(uint32_t playerId);
        Textures LocalTextures();
        // MC DefaultPlayerSkin's Steve / Alex, for anyone without a skin of
        // their own.
        ::Render::TextureHandle DefaultSkin(Game::SkinModel model);

        // ── Per-body animation state (cape physics, swimming) ───────────
        // Once per client tick, after the remote players' own tick: every
        // remote body drawn with the player model.
        void TickRemote(const RemotePlayerManager& players);
        // Once per client tick for the local body (`headYaw` the camera's,
        // degrees): its avatar state plus what a remote copy keeps for it
        // and the local player does not — the lagged body yaw and the limb
        // swing (MC LivingEntity tickHeadTurn / updateWalkAnimation).
        void TickLocal(const glm::dvec3& pos, float headYaw, bool onGround, bool riding,
                       bool swimming, bool inWater);
        struct LocalBody {
            Game::WalkAnimationState walk;
            float bodyYaw = 0.0f, bodyYawO = 0.0f;
            int   ticks = 0;
            bool  initialized = false;
        };
        const LocalBody& LocalBodyState() const { return m_localBody; }
        const AvatarState* RemoteAvatar(uint32_t playerId) const;
        const AvatarState& LocalAvatar() const { return m_local.physics; }
        // The local body's last two tick positions (what extractCapeState
        // lerps between).
        glm::dvec3 LocalPrevPos() const { return m_localPrevPos; }

        // Every GPU texture released (renderer shutdown / backend switch).
        void ReleaseTextures();

    private:
        struct Entry {
            Game::PlayerAppearance appearance;
            ::Render::TextureHandle skin = ::Render::INVALID_TEXTURE;
            ::Render::TextureHandle cape = ::Render::INVALID_TEXTURE;
            bool skinTried = false;
            bool capeTried = false;
            AvatarState physics;
        };

        Textures Resolve(Entry& entry);
        static void Release(::Render::TextureHandle& texture);
        static void ReleaseEntry(Entry& entry);

        Entry m_local;
        LocalBody m_localBody;
        glm::dvec3 m_localPrevPos{0.0};
        std::unordered_map<uint32_t, Entry> m_remote;
        ::Render::TextureHandle m_defaultSkins[2] = { ::Render::INVALID_TEXTURE, ::Render::INVALID_TEXTURE };
        bool m_defaultTried[2] = { false, false };
    };

} // namespace Client
