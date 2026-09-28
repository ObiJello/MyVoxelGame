// File: src/server/level/PatrolSpawner.cpp
//
// See PatrolSpawner.hpp. Reference: minecraft_code_26.3-pre-2/decompiled_net/
// minecraft/world/level/levelgen/PatrolSpawner.java.
#include "server/level/PatrolSpawner.hpp"

#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/LevelEntityStore.hpp"
#include "server/level/ServerLevel.hpp"

#include "common/core/Log.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/entity/raid/Raider.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Server {

    namespace {

        // The overworld's early_game timeline (data/minecraft/timeline/
        // early_game.json, tagged #in_overworld): its
        // gameplay/can_pillager_patrol_spawn track is false from 0 and true
        // from 120000 on the overworld clock, combined with "and".
        constexpr int64_t kEarlyGamePatrolTicks = 120000;

        std::filesystem::path DataRoot() {
            if (const char* env = std::getenv("MC_DATA_ROOT")) return env;
            return "data";
        }

        // The biome's own EnvironmentAttributes.CAN_PILLAGER_PATROL_SPAWN
        // ("attributes" → "minecraft:gameplay/can_pillager_patrol_spawn" in
        // the biome JSON; the attribute's default is true). Read once per
        // biome and cached — vanilla sets it false only for mushroom_fields.
        bool BiomeAllowsPatrols(std::string_view biomeName) {
            static std::mutex s_mutex;
            static std::unordered_map<std::string, bool> s_cache;
            const std::string key(biomeName);
            std::lock_guard<std::mutex> lock(s_mutex);
            if (const auto it = s_cache.find(key); it != s_cache.end()) return it->second;

            std::string ns = "minecraft";
            std::string path = key;
            if (const size_t colon = key.find(':'); colon != std::string::npos) {
                ns = key.substr(0, colon);
                path = key.substr(colon + 1);
            }
            bool allows = true;
            const std::filesystem::path file = DataRoot() / ns / "worldgen" / "biome" / (path + ".json");
            if (std::ifstream in(file); in) {
                try {
                    nlohmann::json biome;
                    in >> biome;
                    if (const auto attributes = biome.find("attributes");
                        attributes != biome.end() && attributes->is_object()) {
                        if (const auto value = attributes->find("minecraft:gameplay/can_pillager_patrol_spawn");
                            value != attributes->end() && value->is_boolean()) {
                            allows = value->get<bool>();
                        }
                    }
                } catch (const std::exception& e) {
                    Log::Warning("[PatrolSpawner] %s: %s", file.string().c_str(), e.what());
                }
            }
            s_cache.emplace(key, allows);
            return allows;
        }

    } // namespace

    void PatrolSpawner::Tick(ServerLevel& level) {
        ServerLevelBridge* bridge = level.MobLevel();
        Game::World* world = level.World();
        if (!bridge || !world) return;

        // spawnEnemies (not Peaceful) and the spawn_patrols rule gate the
        // countdown itself.
        if (bridge->GetDifficulty() == Game::Difficulty::Peaceful) return;
        if (!Game::Rules::GetBool(Game::Rules::Id::SpawnPatrols)) return;

        Game::JavaRandom& random = bridge->Random();
        if (--m_nextTick > 0) return;
        m_nextTick += 12000 + random.NextInt(1200);
        if (!world->IsBrightOutside()) return;
        if (random.NextInt(5) != 0) return;

        // level.players() — every player of the level, spectators included
        // (the pick is made before the spectator test).
        const std::vector<PlayerEntityView*>& players = bridge->PlayerViews();
        const int playerCount = static_cast<int>(players.size());
        if (playerCount < 1) return;
        PlayerEntityView* player = players[static_cast<size_t>(random.NextInt(playerCount))];
        if (!player || player->IsSpectator()) return;

        const glm::ivec3 playerPos = player->BlockPosition();
        if (Game::PoiManager* poi = bridge->GetPoiManager(); poi && poi->IsCloseToVillage(playerPos, 2)) return;

        const int x = (24 + random.NextInt(24)) * (random.NextBool() ? -1 : 1);
        const int z = (24 + random.NextInt(24)) * (random.NextBool() ? -1 : 1);
        glm::ivec3 spawnPos = playerPos + glm::ivec3(x, 0, z);

        // level.hasChunksAt(x - 10, z - 10, x + 10, z + 10).
        for (int cx = (spawnPos.x - 10) >> 4; cx <= (spawnPos.x + 10) >> 4; ++cx) {
            for (int cz = (spawnPos.z - 10) >> 4; cz <= (spawnPos.z + 10) >> 4; ++cz) {
                if (!world->IsChunkLoaded(cx, cz)) return;
            }
        }

        // CAN_PILLAGER_PATROL_SPAWN at spawnPos (still at the player's y):
        // the early_game track, then the biome's own value.
        if (world->GetDayTime() < kEarlyGamePatrolTicks) return;
        const auto biome = world->GetBiome(spawnPos.x, spawnPos.y, spawnPos.z);
        if (!BiomeAllowsPatrols(Game::BiomeRegistry::Get(biome).name)) return;

        const float effective = bridge->GetCurrentDifficultyAt(spawnPos).GetEffectiveDifficulty();
        const int groupSize = static_cast<int>(std::ceil(static_cast<double>(effective))) + 1;

        for (int i = 0; i < groupSize; ++i) {
            // getHeightmapPos(MOTION_BLOCKING_NO_LEAVES, spawnPos).getY().
            spawnPos.y = world->GetSurfaceHeight(spawnPos.x, spawnPos.z, Game::HeightmapType::MotionBlockingNoLeaves) + 1;
            if (i == 0) {
                if (!SpawnPatrolMember(level, spawnPos, true)) break;
            } else {
                SpawnPatrolMember(level, spawnPos, false);
            }
            spawnPos.x += random.NextInt(5) - random.NextInt(5);
            spawnPos.z += random.NextInt(5) - random.NextInt(5);
        }
    }

    bool PatrolSpawner::SpawnPatrolMember(ServerLevel& level, const glm::ivec3& pos, bool isLeader) {
        ServerLevelBridge* bridge = level.MobLevel();
        MobManager* mobs = level.Mobs();
        if (!bridge || !mobs) return false;
        const Game::IBlockAccess* blocks = bridge->Blocks();
        if (!blocks) return false;

        // NaturalSpawner.isValidEmptySpawnBlock(level, pos, state, fluid, PILLAGER).
        if (!Game::IsValidEmptySpawnBlock(Game::EntityTypeId::Pillager, *blocks, pos.x, pos.y, pos.z)) {
            return false;
        }
        // PatrollingMonster.checkPatrollingMonsterSpawnRules(PILLAGER, level,
        // PATROL, pos, random).
        Game::SpawnRuleContext ctx{ *bridge, *blocks, bridge->Random(), Game::SpawnReason::Patrol };
        if (!Game::CheckSpawnRules(Game::EntityTypeId::Pillager, ctx, pos)) return false;

        // EntityTypes.PILLAGER.create(level, PATROL).
        std::unique_ptr<Game::Mob> mob = MakeMobForLoad(Game::EntityTypeId::Pillager, bridge);
        auto* pillager = dynamic_cast<Game::PatrollingMonster*>(mob.get());
        if (!pillager) return false;
        if (isLeader) {
            pillager->SetPatrolLeader(true);
            // Rolled while the new pillager still stands at the origin — MC's
            // order (findPatrolTarget before setPos).
            pillager->FindPatrolTarget();
        }
        // setPos(x, y, z) — the block corner, not its centre.
        pillager->position = glm::dvec3(pos.x, pos.y, pos.z);
        pillager->oldPosition = pillager->position;
        pillager->FinalizeSpawn(Game::SpawnReason::Patrol, nullptr);
        // addFreshEntityWithPassengers — a pillager carries none.
        if (mobs->Add(std::move(mob)) == 0) return false;
        if (isLeader) {
            Log::Info("[PatrolSpawner] patrol leader at %d %d %d", pos.x, pos.y, pos.z);
        }
        return true;
    }

} // namespace Server
