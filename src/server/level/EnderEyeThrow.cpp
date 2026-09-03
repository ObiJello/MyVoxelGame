// File: src/server/level/EnderEyeThrow.cpp
//
// Server half of `Game::ThrowEnderEye` — the common→server bridge declared in
// WorldMobSpawn.hpp, alongside SpawnMobFromItem and for the same reason: the
// Eye of Ender is an ordinary item behaviour, item behaviours live in common,
// and the mob managers live in server.
//
// It gets its own translation unit rather than joining WorldMobSpawn.cpp
// because it needs the dimension array, the stronghold locate and the entity
// type, none of which the spawn-egg bridge has any business including.
//
// Reference: minecraft_code/decompiled_net/minecraft/world/item/EnderEyeItem.java:85-101.

#include "ServerLevel.hpp"
#include "StrongholdLocate.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/projectile/EyeOfEnder.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/core/Mth.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/WorldMobSpawn.hpp"

#include <memory>

namespace Game {

    bool ThrowEnderEye(int dimensionId, const glm::dvec3& from, const ItemStack& stack) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;

        Server::ServerLevel* level =
            server->GetLevel(Game::DimensionFromRaw(dimensionId));
        if (!level || !level->Mobs() || !level->MobLevel()) return false;

        // MC EnderEyeItem.java:85 — findNearestMapStructure(..., 100, false).
        // The 100-chunk cap is on the SEARCH, and a concentric-rings placement
        // has no search: its positions are known from the seed, so the cap has
        // nothing to bound and is dropped here rather than faked.
        const glm::ivec3 around{
            static_cast<int>(std::floor(from.x)),
            static_cast<int>(std::floor(from.y)),
            static_cast<int>(std::floor(from.z)),
        };
        const auto stronghold = Server::FindNearestStronghold(*level, around);
        if (!stronghold) {
            // MC returns CONSUME here with the stack untouched — see the
            // declaration. The caller must not shrink on false.
            return false;
        }

        auto eye = std::make_unique<EyeOfEnder>(level->MobLevel());
        eye->position = from;
        eye->SetItem(stack);
        // MC Vec3.atLowerCornerOf(nearestMapFeature) — the block's corner, not
        // its centre, and its y is 0. Neither matters: signalTo's 12-block
        // clamp replaces the target with a proxy in that direction.
        eye->SignalTo(glm::dvec3(stronghold->x, stronghold->y, stronghold->z));

        if (level->Mobs()->Add(std::move(eye)) == 0) {
            Log::Warning("[EnderEye] Could not register the thrown eye");
            return false;
        }
        return true;
    }

    bool ThrowEnderPearl(int dimensionId, IUsePlayer& player) {
        // MC EnderpearlItem.use's server branch:
        //   Projectile.spawnProjectileFromRotation(ThrownEnderpearl::new,
        //       level, stack, player, 0.0F, 1.5F, 1.0F)
        // — spawn at the eye minus 0.1, aim from the player's rotation, power
        // 1.5, inaccuracy 1.0. The owner is the player's entity VIEW, so the
        // pearl's on-hit can pull the thrower through the bridge's
        // TeleportPlayer.
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;

        Server::ServerLevel* level =
            server->GetLevel(Game::DimensionFromRaw(dimensionId));
        if (!level || !level->Mobs() || !level->MobLevel()) return false;

        auto* serverPlayer = dynamic_cast<Server::ServerPlayer*>(&player);
        if (!serverPlayer) return false;
        // Views are keyed by CONNECTION id, not player id — resolve through
        // the session rather than assuming the two coincide.
        auto* sessions = server->GetSessionManager();
        auto session = sessions
            ? sessions->GetSession(serverPlayer->getPlayerId()) : nullptr;
        Server::PlayerEntityView* view = session
            ? level->MobLevel()->GetPlayerView(session->GetConnectionId())
            : nullptr;
        if (!view) return false;

        auto pearl = std::make_unique<ThrownEnderpearl>(level->MobLevel());
        pearl->SetOwnerAndPosition(*view);
        // MC Projectile.spawnProjectileFromRotation(ThrownEnderpearl::new,
        // level, stack, player, 0.0F, 1.5F, 1.0F) — the view angles become
        // the direction and the thrower's own movement is added on top (the
        // view reports the client's last displacement as known movement).
        pearl->ShootFromRotation(*view, player.getPitch(), player.getYaw(),
                                 0.0f, 1.5f, 1.0f);

        return level->Mobs()->Add(std::move(pearl)) != 0;
    }

} // namespace Game
