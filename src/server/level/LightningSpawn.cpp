// File: src/server/level/LightningSpawn.cpp
#include "server/level/LightningSpawn.hpp"

#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"

#include "common/entity/LightningBolt.hpp"

#include <memory>

namespace Server {

    Game::LightningBolt* SpawnLightningBolt(ServerLevel& level, const glm::dvec3& pos,
                                            bool visualOnly) {
        ServerLevelBridge* bridge = level.MobLevel();
        if (!bridge) return nullptr;

        // MC EntityType.LIGHTNING_BOLT.create + snapTo + setVisualOnly.
        auto bolt = std::make_unique<Game::LightningBolt>(bridge);
        bolt->position = pos;
        bolt->oldPosition = pos;
        bolt->SetVisualOnly(visualOnly);

        Game::LightningBolt* raw = bolt.get();
        // MC level.addFreshEntity(bolt).
        bridge->AddFreshEntity(std::move(bolt));
        return raw;
    }

} // namespace Server
