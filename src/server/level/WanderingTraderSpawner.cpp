// File: src/server/level/WanderingTraderSpawner.cpp
#include "server/level/WanderingTraderSpawner.hpp"

#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/LevelEntityStore.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/world/storage/NBTParser.hpp"

#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/SpawnReason.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/entity/ai/village/PoiTypes.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/npc/WanderingTrader.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"
#include "levelgen/structure/StructureSet.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace Server {

    namespace {

        namespace mls = minecraft::levelgen::structure;

        // MC BiomeTags.WITHOUT_WANDERING_TRADER_SPAWNS, from the data pack
        // (vanilla: minecraft:the_void). Resolved once; a missing tag file
        // reads as an empty tag.
        const std::unordered_set<std::string>& WithoutTraderSpawnBiomes() {
            static const std::unordered_set<std::string> s_biomes = [] {
                try {
                    return mls::BiomeTags::resolve("#minecraft:without_wandering_trader_spawns");
                } catch (const std::exception& e) {
                    Log::Warning("[WanderingTrader] biome tag unavailable: %s", e.what());
                    return std::unordered_set<std::string>{};
                }
            }();
            return s_biomes;
        }

        bool BiomeForbidsTraders(std::string_view name) {
            const std::string id = name.find(':') == std::string_view::npos
                ? "minecraft:" + std::string(name) : std::string(name);
            return WithoutTraderSpawnBiomes().count(id) != 0;
        }

        int64_t NewSpawnerSeed() {
            // MC RandomSource.create(): a seed from the clock.
            return static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^
                static_cast<int64_t>(0x5DEECE66DLL);
        }

    } // namespace

    WanderingTraderSpawner::WanderingTraderSpawner() : m_random(NewSpawnerSeed()) {}

    void WanderingTraderSpawner::Load(int spawnDelay, int spawnChance) {
        m_spawnDelay  = spawnDelay;
        m_spawnChance = spawnChance;
    }

    bool WanderingTraderSpawner::LoadSavedDataFile(const std::filesystem::path& file) {
        // MC SavedDataStorage: gzip NBT {"": {data: {...}, DataVersion}}.
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) return false;
        std::ifstream f(file, std::ios::binary);
        if (!f) return false;
        const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> nbt;
        if (raw.empty() || !Game::Nbt::GzipDecompress(raw, nbt)) return false;
        auto root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(nbt));
        if (!root) return false;
        auto data = std::dynamic_pointer_cast<::World::NBTTagCompound>(root->GetTag("data"));
        if (!data) return false;
        // WanderingTraderData.CODEC: optionalFieldOf with 24000 / 25.
        m_spawnDelay  = data->GetValue<int32_t>("spawn_delay", kDefaultSpawnDelay);
        m_spawnChance = data->GetValue<int32_t>("spawn_chance", kMinSpawnChance);
        return true;
    }

    void WanderingTraderSpawner::Tick(ServerLevel& level) {
        // MC WanderingTraderSpawner.tick.
        if (!Game::Rules::GetBool(Game::Rules::Id::SpawnWanderingTraders)) return;
        if (--m_tickDelay > 0) return;
        m_tickDelay = kDefaultTickDelay;
        m_spawnDelay -= kDefaultTickDelay;
        if (m_spawnDelay > 0) return;
        m_spawnDelay = kDefaultSpawnDelay;
        const int chanceToSpawn = m_spawnChance;
        m_spawnChance = std::clamp(chanceToSpawn + kSpawnChanceIncrease, kMinSpawnChance, kMaxSpawnChance);
        if (m_random.NextInt(100) > chanceToSpawn) return;
        if (Spawn(level)) m_spawnChance = kMinSpawnChance;
    }

    bool WanderingTraderSpawner::Spawn(ServerLevel& level) {
        ServerLevelBridge* bridge = level.MobLevel();
        MobManager* mobs = level.Mobs();
        if (!bridge || !mobs || !level.World()) return false;

        // MC ServerLevel.getRandomPlayer: a uniform pick (the LEVEL's random)
        // among the alive players. No player → "spawned" (the chance resets).
        std::vector<PlayerEntityView*> alive;
        for (PlayerEntityView* view : bridge->PlayerViews()) {
            if (view && view->IsAlive()) alive.push_back(view);
        }
        if (alive.empty()) return true;
        PlayerEntityView* player = alive[static_cast<size_t>(
            bridge->Random().NextInt(static_cast<int>(alive.size())))];

        if (m_random.NextInt(kSpawnOneInXChance) != 0) return false;

        const glm::ivec3 playerPos = player->BlockPosition();
        // PoiManager.find(MEETING, any, playerPos, 48, ANY): the FIRST record
        // in getInRange's walk order, not the closest.
        glm::ivec3 referencePos = playerPos;
        if (Game::PoiManager* poi = bridge->GetPoiManager()) {
            const auto meetings = poi->GetInRange(
                [](Game::PoiType t) { return t == Game::PoiType::Meeting; },
                playerPos, kSearchRadius, Game::PoiManager::Occupancy::Any);
            if (!meetings.empty() && meetings.front()) referencePos = meetings.front()->pos;
        }

        const std::optional<glm::ivec3> spawnPos = FindSpawnPositionNear(level, referencePos, kSearchRadius);
        if (!spawnPos || !HasEnoughSpace(level, *spawnPos)) return false;
        const auto biome = level.World()->GetBiome(spawnPos->x, spawnPos->y, spawnPos->z);
        if (BiomeForbidsTraders(Game::BiomeRegistry::Get(biome).name)) return false;

        // EntityTypes.WANDERING_TRADER.spawn(level, pos, EVENT).
        std::unique_ptr<Game::Mob> mob = MakeMobForLoad(Game::EntityTypeId::WanderingTrader, bridge);
        auto* trader = dynamic_cast<Game::WanderingTrader*>(mob.get());
        if (!trader) return false;
        trader->position = glm::dvec3(spawnPos->x + 0.5, spawnPos->y, spawnPos->z + 0.5);
        trader->oldPosition = trader->position;
        trader->yRot = Game::Mth::WrapDegrees(bridge->Random().NextFloat() * 360.0f);
        trader->xRot = 0.0f;
        trader->SetYHeadRot(trader->yRot);
        trader->yBodyRot = trader->yRot;
        trader->FinalizeSpawn(Game::SpawnReason::Event, nullptr);
        const int32_t traderId = mobs->Add(std::move(mob));
        if (traderId == 0) return false;
        trader->PlayAmbientSound();

        for (int i = 0; i < 2; ++i) TryToSpawnLlamaFor(level, traderId, trader->BlockPosition());

        trader->SetDespawnDelay(Game::WanderingTrader::kSpawnedDespawnDelay);
        trader->SetWanderTarget(referencePos);
        trader->SetHomeTo(referencePos, kHomeRadius);
        Log::Info("[WanderingTrader] spawned at %d %d %d (reference %d %d %d)",
                  spawnPos->x, spawnPos->y, spawnPos->z,
                  referencePos.x, referencePos.y, referencePos.z);
        return true;
    }

    void WanderingTraderSpawner::TryToSpawnLlamaFor(ServerLevel& level, int32_t traderId,
                                                    const glm::ivec3& traderPos) {
        ServerLevelBridge* bridge = level.MobLevel();
        MobManager* mobs = level.Mobs();
        const std::optional<glm::ivec3> spawnPos = FindSpawnPositionNear(level, traderPos, kLlamaRadius);
        if (!spawnPos) return;
        std::unique_ptr<Game::Mob> mob = MakeMobForLoad(Game::EntityTypeId::TraderLlama, bridge);
        Game::Mob* llama = mob.get();
        if (!llama) return;
        llama->position = glm::dvec3(spawnPos->x + 0.5, spawnPos->y, spawnPos->z + 0.5);
        llama->oldPosition = llama->position;
        llama->yRot = Game::Mth::WrapDegrees(bridge->Random().NextFloat() * 360.0f);
        llama->xRot = 0.0f;
        llama->SetYHeadRot(llama->yRot);
        llama->yBodyRot = llama->yRot;
        llama->FinalizeSpawn(Game::SpawnReason::Event, nullptr);
        if (mobs->Add(std::move(mob)) == 0) return;
        llama->PlayAmbientSound();
        // llama.setLeashedTo(trader, true) — the tracker sends the link.
        if (Game::Mob* trader = mobs->Find(traderId)) llama->SetLeashedTo(*trader, true);
    }

    std::optional<glm::ivec3> WanderingTraderSpawner::FindSpawnPositionNear(
            ServerLevel& level, const glm::ivec3& reference, int radius) {
        // MC findSpawnPositionNear: ten tries at a random column within
        // `radius` (nextInt(2r) - r on each axis), on the wandering trader's
        // heightmap (MOTION_BLOCKING_NO_LEAVES) and ON_GROUND placement.
        Game::World* world = level.World();
        ServerLevelBridge* bridge = level.MobLevel();
        const Game::IBlockAccess* blocks = bridge ? bridge->Blocks() : nullptr;
        if (!world || !blocks) return std::nullopt;
        for (int i = 0; i < kNumberOfSpawnAttempts; ++i) {
            const int x = reference.x + m_random.NextInt(radius * 2) - radius;
            const int z = reference.z + m_random.NextInt(radius * 2) - radius;
            // An unloaded column has no heightmap to read (MC would load it);
            // no trader stands on ground the server does not have.
            if (!world->IsChunkLoaded(x >> 4, z >> 4)) continue;
            // Level.getHeight = the first cell above the column's top block.
            const int y = world->GetSurfaceHeight(x, z, Game::HeightmapType::MotionBlockingNoLeaves) + 1;
            if (Game::IsSpawnPositionOk(Game::EntityTypeId::WanderingTrader, *blocks, x, y, z)) {
                return glm::ivec3(x, y, z);
            }
        }
        return std::nullopt;
    }

    bool WanderingTraderSpawner::HasEnoughSpace(ServerLevel& level, const glm::ivec3& pos) const {
        // MC hasEnoughSpace: every block in [pos, pos + (1, 2, 1)] has an
        // empty collision shape.
        ServerLevelBridge* bridge = level.MobLevel();
        const Game::IBlockAccess* blocks = bridge ? bridge->Blocks() : nullptr;
        if (!blocks) return false;
        for (int dx = 0; dx <= 1; ++dx) {
            for (int dy = 0; dy <= 2; ++dy) {
                for (int dz = 0; dz <= 1; ++dz) {
                    const Game::BlockState state = blocks->GetBlockState(pos.x + dx, pos.y + dy, pos.z + dz);
                    if (!Game::BlockRegistry::HasCollision(state.Block())) continue;
                    if (Game::BlockRegistry::GetBlockCollisionShapeSet(state).count != 0) return false;
                }
            }
        }
        return true;
    }

} // namespace Server
