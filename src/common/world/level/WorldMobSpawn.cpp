// File: src/common/world/level/WorldMobSpawn.cpp
#include "WorldMobSpawn.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"

namespace Game {

    bool SpawnMobFromItem(EntityTypeId type, const glm::ivec3& spawnPos,
                          bool tryMoveDown, bool movedUp, DimensionId dimension,
                          int portalCooldownTicks,
                          const std::function<void(Mob&)>& configure,
                          SpawnReason reason) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;
        return server->SpawnMobFromItemUse(type, spawnPos, tryMoveDown, movedUp, dimension,
                                           portalCooldownTicks, configure, reason);
    }

    bool AnyPlayerCloseEnoughForSpawning(DimensionId dimension, const glm::ivec3& pos) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;
        Server::ServerLevel* level = server->GetLevel(dimension);
        Server::ServerLevelBridge* bridge = level ? level->MobLevel() : nullptr;
        if (!bridge) return false;
        // euclideanDistanceSquared(chunkPos, player): chunk centre to the
        // player, XZ only, < 128².
        const double centerX = static_cast<double>(pos.x >> 4) * 16.0 + 8.0;
        const double centerZ = static_cast<double>(pos.z >> 4) * 16.0 + 8.0;
        for (const Server::PlayerEntityView* player : bridge->PlayerViews()) {
            if (!player || player->IsSpectator()) continue;
            const double dx = player->position.x - centerX;
            const double dz = player->position.z - centerZ;
            if (dx * dx + dz * dz < 16384.0) return true;
        }
        return false;
    }

} // namespace Game
