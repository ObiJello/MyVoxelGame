// File: src/common/world/level/WorldFallingBlock.cpp
#include "common/world/level/WorldFallingBlock.hpp"

#include "common/entity/FallingBlockEntity.hpp"
#include "common/world/level/World.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/FallingBlockStore.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"

namespace Game {

    FallingBlockEntity* SpawnFallingBlock(ILevelWrite& level,
                                          const glm::ivec3& blockPos,
                                          BlockState state) {
        auto* server = Server::g_integratedServer.get();
        if (!server) return nullptr;

        // Which dimension is this? Matched on the World POINTER rather than on
        // the dimension id: pointer identity cannot be wrong, and a block tick
        // running against a level that is not one of the server's (a test
        // harness, the client's predicted world) correctly finds nothing.
        auto* world = dynamic_cast<World*>(&level);
        if (!world) return nullptr;

        Server::ServerLevel* serverLevel = server->GetLevel(world->GetDimension());
        if (!serverLevel || serverLevel->World() != world) return nullptr;

        Server::ServerLevelBridge* bridge = serverLevel->MobLevel();
        Server::MobManager*        mobs   = serverLevel->Mobs();
        if (!bridge || !mobs) return nullptr;

        // Plain sand/gravel takes the compact representation — no Mob object
        // at all. The callers that need the returned pointer (anvil fall
        // damage, suspicious-block cancelDrop, dripstone) never pass a state
        // the store takes, so null is the correct answer for them here.
        if (Server::FallingBlockStore* store = serverLevel->FallingBlocks();
            store && Server::FallingBlockStore::Takes(state)) {
            store->Spawn(blockPos, state);
            return nullptr;
        }

        auto entity = std::make_unique<FallingBlockEntity>(bridge);
        entity->InitFall(blockPos, state);

        // Straight into the manager rather than through the bridge's deferred
        // AddFreshEntity queue: the caller needs the pointer back THIS call to
        // arm an anvil's fall damage, and a queued spawn has not been given an
        // id yet. Safe because a block tick is not iterating the mob container
        // — that is the case the deferral exists for.
        FallingBlockEntity* raw = entity.get();
        if (mobs->Add(std::move(entity)) == 0) return nullptr;
        return raw;
    }

} // namespace Game
