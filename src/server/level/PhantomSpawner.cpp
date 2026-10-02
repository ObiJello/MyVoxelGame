// File: src/server/level/PhantomSpawner.cpp
//
// See PhantomSpawner.hpp. Reference: minecraft_code_26.3-pre-2/decompiled_net/
// minecraft/world/level/levelgen/PhantomSpawner.java.
#include "server/level/PhantomSpawner.hpp"

#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/LevelEntityStore.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/player/ServerPlayer.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace Server {

    void PhantomSpawner::Tick(ServerLevel& level) {
        ServerLevelBridge* bridge = level.MobLevel();
        MobManager* mobs = level.Mobs();
        Game::World* world = level.World();
        if (!bridge || !mobs || !world) return;

        // spawnEnemies (not Peaceful), then the spawn_phantoms rule, gate the
        // countdown itself.
        if (bridge->GetDifficulty() == Game::Difficulty::Peaceful) return;
        if (!Game::Rules::GetBool(Game::Rules::Id::SpawnPhantoms)) return;

        Game::JavaRandom& random = bridge->Random();
        if (--m_nextTick > 0) return;
        m_nextTick += (60 + random.NextInt(60)) * 20;

        const bool hasSkyLight = Game::DimensionHasSkyLight(level.Dimension());
        if (world->GetSkyDarken() < 5 && hasSkyLight) return;

        const Game::IBlockAccess* blocks = bridge->Blocks();
        if (!blocks) return;
        const int seaLevel = Game::DimensionSeaLevel(level.Dimension());

        // level.players(), in order. Copied: a spawn below never changes the
        // player list, but the loop must not depend on that.
        const std::vector<PlayerEntityView*> players = bridge->PlayerViews();
        for (PlayerEntityView* view : players) {
            if (!view || view->IsSpectator()) continue;
            ServerPlayer* player = view->GetPlayer();
            if (!player) continue;

            const glm::ivec3 playerPos = view->BlockPosition();
            if (hasSkyLight &&
                (playerPos.y < seaLevel || !world->CanSeeSky(playerPos.x, playerPos.y, playerPos.z))) {
                continue;
            }

            const Game::DifficultyInstance difficulty = bridge->GetCurrentDifficultyAt(playerPos);
            if (!difficulty.IsHarderThan(random.NextFloat() * 3.0f)) continue;

            // Stats.TIME_SINCE_REST, clamped to [1, Integer.MAX_VALUE]; three
            // days (72000 ticks) awake before the roll can succeed at all.
            const int timeSinceRest = std::max(player->getTimeSinceRest(), 1);
            if (random.NextInt(timeSinceRest) < 72000) continue;

            // playerPos.above(20 + nextInt(15)).east(-10 + nextInt(21))
            // .south(-10 + nextInt(21)) — Java's left-to-right order.
            const int up    = 20 + random.NextInt(15);
            const int east  = -10 + random.NextInt(21);
            const int south = -10 + random.NextInt(21);
            const glm::ivec3 spawnPos = playerPos + glm::ivec3(east, up, south);
            if (!Game::IsValidEmptySpawnBlock(Game::EntityTypeId::Phantom, *blocks,
                                              spawnPos.x, spawnPos.y, spawnPos.z)) {
                continue;
            }

            std::shared_ptr<Game::SpawnGroupData> groupData;
            const int groupSize = 1 + random.NextInt(static_cast<int>(difficulty.GetDifficulty()) + 1);
            for (int i = 0; i < groupSize; ++i) {
                // EntityTypes.PHANTOM.create(level, NATURAL).
                std::unique_ptr<Game::Mob> phantom = MakeMobForLoad(Game::EntityTypeId::Phantom, bridge);
                if (!phantom) continue;
                // snapTo(spawnPos, 0, 0): the cell's bottom centre.
                phantom->position = glm::dvec3(spawnPos.x + 0.5, spawnPos.y, spawnPos.z + 0.5);
                phantom->oldPosition = phantom->position;
                phantom->yRot = 0.0f;
                phantom->xRot = 0.0f;
                groupData = phantom->FinalizeSpawn(Game::SpawnReason::Natural, groupData);
                // addFreshEntityWithPassengers — a phantom carries none.
                mobs->Add(std::move(phantom));
            }
        }
    }

} // namespace Server
