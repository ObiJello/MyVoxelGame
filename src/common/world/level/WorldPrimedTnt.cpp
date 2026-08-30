// File: src/common/world/level/WorldPrimedTnt.cpp
#include "common/world/level/WorldPrimedTnt.hpp"

#include "common/entity/PrimedTnt.hpp"
#include "common/world/level/World.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"

namespace Game {

    bool SpawnPrimedTnt(ILevelWrite& level, const glm::ivec3& blockPos, Entity* igniter) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return false;

        auto* world = dynamic_cast<World*>(&level);
        if (!world) return false;
        // MC gates prime() on this rule and returns false when it is off,
        // which is what lets TntUseItemOn distinguish "disabled" from "worked".
        if (!world->GetTntExplodes()) return false;

        Server::ServerLevel* serverLevel = server->GetLevel(world->GetDimension());
        if (!serverLevel || serverLevel->World() != world) return false;

        Server::ServerLevelBridge* bridge = serverLevel->MobLevel();
        Server::MobManager*        mobs   = serverLevel->Mobs();
        if (!bridge || !mobs) return false;

        auto tnt = std::make_unique<PrimedTnt>(bridge);
        // MC primes at (x + 0.5, y, z + 0.5) — the cell's horizontal centre,
        // sitting on its floor.
        tnt->InitPrimed(glm::dvec3(static_cast<double>(blockPos.x) + 0.5,
                                   static_cast<double>(blockPos.y),
                                   static_cast<double>(blockPos.z) + 0.5),
                        igniter);
        return mobs->Add(std::move(tnt)) != 0;
    }

} // namespace Game
