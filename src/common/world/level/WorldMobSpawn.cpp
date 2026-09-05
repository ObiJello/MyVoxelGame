// File: src/common/world/level/WorldMobSpawn.cpp
#include "WorldMobSpawn.hpp"
#include "server/IntegratedServer.hpp"

namespace Game {

    bool SpawnMobFromItem(EntityTypeId type, const glm::ivec3& spawnPos,
                          bool tryMoveDown, bool movedUp, DimensionId dimension,
                          int portalCooldownTicks,
                          const std::function<void(Mob&)>& configure) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;
        return server->SpawnMobFromItemUse(type, spawnPos, tryMoveDown, movedUp, dimension,
                                           portalCooldownTicks, configure);
    }

} // namespace Game
