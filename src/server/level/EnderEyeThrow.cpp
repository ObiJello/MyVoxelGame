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

} // namespace Game
