// File: src/client/sound/LocalPlayerSounds.cpp
#include "client/sound/LocalPlayerSounds.hpp"

#include "client/entity/Player.hpp"
#include "client/sound/ClientSounds.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/PlayerMovementSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace Client::LocalPlayerSounds {

    namespace {
        Game::PlayerMovementSounds g_movement;
        Game::JavaRandom           g_random(
            static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        bool       g_hasPrevious = false;
        glm::dvec3 g_previousPosition{0.0};
        float      g_portalEffectIntensity = 0.0f;
        bool       g_inPortalLastTick = false;

        // MC Portal.Transition.CONFUSION: the portals whose hum and warp
        // build while you stand in them. The Hush and Aether portals are
        // nether-portal kin (PortalFamily); the End and Twilight portals are
        // instant.
        bool IsConfusionPortal(Game::BlockID b) {
            return b == Game::BlockID::NetherPortal || b == Game::BlockID::HushPortal ||
                   b == Game::BlockID::AetherPortal;
        }
        bool IsAnyPortal(Game::BlockID b) {
            return IsConfusionPortal(b) || b == Game::BlockID::EndPortal || b == Game::BlockID::TwilightPortal;
        }

        // Every cell the body overlaps (MC checkInsideBlocks), tested.
        template <typename Pred>
        bool BodyTouches(const Game::IBlockAccess& blocks, const Game::PlayerPhysics& ph, Pred pred) {
            const double hw = ph.GetWidth() * 0.5;
            const double h  = ph.GetCurrentHeight();
            const int x0 = static_cast<int>(std::floor(ph.position.x - hw + 1.0e-6));
            const int x1 = static_cast<int>(std::floor(ph.position.x + hw - 1.0e-6));
            const int y0 = static_cast<int>(std::floor(ph.position.y + 1.0e-6));
            const int y1 = static_cast<int>(std::floor(ph.position.y + h - 1.0e-6));
            const int z0 = static_cast<int>(std::floor(ph.position.z - hw + 1.0e-6));
            const int z1 = static_cast<int>(std::floor(ph.position.z + hw - 1.0e-6));
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    for (int z = z0; z <= z1; ++z)
                        if (pred(blocks.GetBlock(x, y, z))) return true;
            return false;
        }
    } // namespace

    void Reset() {
        g_movement.Reset();
        g_hasPrevious = false;
        g_portalEffectIntensity = 0.0f;
        g_inPortalLastTick = false;
    }

    float PortalEffectIntensity() { return g_portalEffectIntensity; }

    void OnDimensionChanged() {
        // MC level event 1032 (ServerPlayer's portal teleport → LevelEventHandler):
        // the whoosh of arriving, only for a trip through a portal.
        if (g_inPortalLastTick) {
            GetSoundManager().Play(SimpleSoundInstance::ForLocalAmbience(
                Game::SoundEvents::PORTAL_TRAVEL, g_random.NextFloat() * 0.4f + 0.8f, 0.25f));
        }
        g_movement.Reset();
        g_hasPrevious = false;
    }

    void Tick(const SoundHost::TickContext& ctx) {
        const Game::ClientPlayer* player = ctx.player;
        if (!player || !ctx.blocks) return;
        // MC LocalPlayer.tick runs only once the client's level is loaded.
        if (ctx.levelLoading) {
            g_hasPrevious = false;
            return;
        }
        const Game::PlayerPhysics& ph = player->physics;
        const Game::IBlockAccess& blocks = *ctx.blocks;
        const bool spectator = player->IsSpectator();

        // ── Movement (Entity.move for the local player) ─────────────────
        if (!g_hasPrevious) {
            g_previousPosition = ph.position;
            g_hasPrevious = true;
        }
        Game::PlayerMovementSounds::Input in;
        in.previousPosition = g_previousPosition;
        in.position = ph.position;
        in.onGround = ph.isOnGround;
        in.crouching = ph.isSneaking;
        in.flying = ph.isFlying;
        in.noPhysics = spectator || ph.noclip || player->IsSleeping();
        in.inWater = ph.isInWater;
        in.swimming = ph.isSprinting && ph.isEyeInWater;
        g_previousPosition = ph.position;

        std::vector<Game::PlayerMovementSound> sounds;
        g_movement.Tick(blocks, in, g_random, sounds);

        // ── Landing (LivingEntity.causeFallDamage) ──────────────────────
        // Player.causeFallDamage: a player who may fly (creative) never
        // takes fall damage and makes no sound of it.
        if (player->landedFallForSound > 0.0f) {
            const float fall = player->landedFallForSound;
            player->landedFallForSound = 0.0f;
            if (!ph.mayFly && !spectator && !ph.isFlying && !ph.isInWater) {
                // SAFE_FALL_DISTANCE: 3, and JUMP_BOOST's +1 per level.
                float safe = 3.0f;
                if (const Game::MobEffectInstance* jump = player->GetEffect(Game::MobEffectId::JumpBoost)) {
                    safe += static_cast<float>(jump->amplifier + 1);
                }
                Game::PlayerMovementSounds::Landing(blocks, ph.position, fall, ph.scale, safe, sounds);
            }
        }

        // LocalPlayer.playSound: level.playLocalSound at the feet, in the
        // PLAYERS category.
        for (const Game::PlayerMovementSound& s : sounds) {
            Sounds::PlayLocal(ph.position, s.event, Game::SoundSource::Players, s.volume, s.pitch, false);
        }

        // ── Portal hum (LocalPlayer.handlePortalTransitionEffect) ───────
        const bool inConfusion = !spectator && BodyTouches(blocks, ph, IsConfusionPortal);
        g_inPortalLastTick = !spectator && (inConfusion || BodyTouches(blocks, ph, IsAnyPortal));
        float step = 0.0f;
        if (inConfusion) {
            if (g_portalEffectIntensity == 0.0f) {
                GetSoundManager().Play(SimpleSoundInstance::ForLocalAmbience(
                    Game::SoundEvents::PORTAL_TRIGGER, g_random.NextFloat() * 0.4f + 0.8f, 0.25f));
            }
            step = 0.0125f;
        } else if (g_portalEffectIntensity > 0.0f) {
            step = -0.05f;
        }
        g_portalEffectIntensity = std::clamp(g_portalEffectIntensity + step, 0.0f, 1.0f);
    }

} // namespace Client::LocalPlayerSounds
