// File: src/server/level/ServerWeather.cpp
//
// See ServerWeather.hpp.
#include "server/level/ServerWeather.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/LevelEntityStore.hpp"   // MakeMobForLoad
#include "server/level/LightningSpawn.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "common/nbt/NbtWrite.hpp"   // Nbt::GzipDecompress

#include "common/core/Profiling_Tracy.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/ai/village/PoiManager.hpp"
#include "common/entity/ai/village/PoiTypes.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/GameEventS2CPacket.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/lighting/ChunkLight.hpp"   // Lighting::LightLayer
#include "common/world/math/WorldCoordinates.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <unordered_set>

namespace Server {

    namespace {

        using Network::GameEventS2CPacket;

        void SendEvent(ServerConnection* connection, uint8_t event, float param) {
            if (!connection) return;
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::WeatherChange),
                                   Network::Serialization::Serialize(GameEventS2CPacket(event, param)));
        }

        // Whose weather a player's client keeps: their own level's when it
        // can have weather, else the Overworld's — the level that drives the
        // shared cycle — so a portal view from the Nether (or the End, the
        // Hush) into a weather level shows its rain (Client::ClientWeather).
        Game::DimensionId WeatherSourceOf(Game::DimensionId playerDimension) {
            return Game::DimensionCanHaveWeather(playerDimension) ? playerDimension
                                                                  : Game::DimensionId::Overworld;
        }

        // MC PlayerList.broadcastAll(packet, dimension) — to the players
        // whose weather source is that level.
        void BroadcastToDimension(PlayerSessionManager* sessions, Game::DimensionId dimension,
                                  uint8_t event, float param) {
            if (!sessions) return;
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session || !session->GetConnection()) continue;
                const Game::DimensionId playerDim =
                    Game::DimensionFromRaw(static_cast<int8_t>(session->GetDimensionId()));
                if (WeatherSourceOf(playerDim) != dimension) continue;
                SendEvent(session->GetConnection(), event, param);
            }
        }

        // MC BlockTags.LIGHTNING_RODS — the copper family's rods.
        bool IsLightningRod(Game::BlockID id) {
            using B = Game::BlockID;
            switch (id) {
                case B::LightningRod:            case B::ExposedLightningRod:
                case B::WeatheredLightningRod:   case B::OxidizedLightningRod:
                case B::WaxedLightningRod:       case B::WaxedExposedLightningRod:
                case B::WaxedWeatheredLightningRod: case B::WaxedOxidizedLightningRod:
                    return true;
                default:
                    return false;
            }
        }

        // MC Level.getHeightmapPos(type, pos): the column's first free Y.
        glm::ivec3 HeightmapPos(const Game::World& world, Game::HeightmapType type, const glm::ivec3& pos) {
            return glm::ivec3(pos.x, world.GetSurfaceHeight(pos.x, pos.z, type) + 1, pos.z);
        }

    } // namespace

    ServerWeather::ServerWeather() {
        // MC's level random is RandomSource.create() — unseeded.
        m_random.SetSeed(static_cast<int64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x5DEECE66DLL);
    }

    int ServerWeather::SampleRainDelay()       { return m_random.NextInt(kRainDelayMin, kRainDelayMax); }
    int ServerWeather::SampleRainDuration()    { return m_random.NextInt(kRainDurationMin, kRainDurationMax); }
    int ServerWeather::SampleThunderDuration() { return m_random.NextInt(kThunderDurationMin, kThunderDurationMax); }

    bool ServerWeather::LoadSavedDataFile(const std::filesystem::path& file) {
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
        m_data.clearWeatherTime = data->GetValue<int32_t>("clear_weather_time", 0);
        m_data.rainTime         = data->GetValue<int32_t>("rain_time", 0);
        m_data.thunderTime      = data->GetValue<int32_t>("thunder_time", 0);
        m_data.raining          = data->GetValue<int8_t>("raining", 0) != 0;
        m_data.thundering       = data->GetValue<int8_t>("thundering", 0) != 0;
        return true;
    }

    void ServerWeather::PrepareLevel(Game::World& world) const {
        // MC ServerLevel.prepareWeather — only for a level that can have
        // weather (the constructor's canHaveWeather gate).
        if (!world.CanHaveWeather()) return;
        if (m_data.raining) {
            world.SetRainLevel(1.0f);
            if (m_data.thundering) world.SetThunderLevel(1.0f);
        }
    }

    void ServerWeather::SetWeatherParameters(int clearTime, int rainTime, bool raining, bool thundering) {
        m_data.clearWeatherTime = clearTime;
        m_data.rainTime         = rainTime;
        m_data.thunderTime      = rainTime;   // sic — MinecraftServer.setWeatherParameters
        m_data.raining          = raining;
        m_data.thundering       = thundering;
    }

    void ServerWeather::ResetWeatherCycle() {
        m_data.rainTime    = 0;
        m_data.raining     = false;
        m_data.thunderTime = 0;
        m_data.thundering  = false;
    }

    void ServerWeather::AdvanceCycle() {
        // MC ServerLevel.advanceWeatherCycle, the advance_weather branch.
        int  clearWeatherTime = m_data.clearWeatherTime;
        int  thunderTime      = m_data.thunderTime;
        int  rainTime         = m_data.rainTime;
        bool thundering       = m_data.thundering;
        bool raining          = m_data.raining;
        if (clearWeatherTime > 0) {
            --clearWeatherTime;
            thunderTime = thundering ? 0 : 1;
            rainTime    = raining ? 0 : 1;
            thundering  = false;
            raining     = false;
        } else {
            if (thunderTime > 0) {
                --thunderTime;
                if (thunderTime == 0) thundering = !thundering;
            } else if (thundering) {
                thunderTime = m_random.NextInt(kThunderDurationMin, kThunderDurationMax);
            } else {
                thunderTime = m_random.NextInt(kThunderDelayMin, kThunderDelayMax);
            }

            if (rainTime > 0) {
                --rainTime;
                if (rainTime == 0) raining = !raining;
            } else if (raining) {
                rainTime = m_random.NextInt(kRainDurationMin, kRainDurationMax);
            } else {
                rainTime = m_random.NextInt(kRainDelayMin, kRainDelayMax);
            }
        }
        m_data.thunderTime      = thunderTime;
        m_data.rainTime         = rainTime;
        m_data.clearWeatherTime = clearWeatherTime;
        m_data.thundering       = thundering;
        m_data.raining          = raining;
    }

    void ServerWeather::Tick(IntegratedServer& server) {
        PROFILE_ZONE_N("Weather.Tick");
        // The shared cycle, once (see the header).
        if (Game::Rules::GetBool(Game::Rules::Id::AdvanceWeather)) AdvanceCycle();

        PlayerSessionManager* sessions = server.GetSessionManager();
        server.ForEachLevel([&](ServerLevel& level) {
            Game::World* world = level.World();
            if (!world || !world->CanHaveWeather()) return;

            // The second half of advanceWeatherCycle: the level's eased
            // levels, and what the clients in it are told.
            const bool  wasRaining  = world->IsRaining();
            const float prevRain    = world->GetRainLevel(1.0f);
            const float prevThunder = world->GetRawThunderLevel();
            world->StepWeatherLevels(m_data.raining, m_data.thundering);
            const float rain    = world->GetRainLevel(1.0f);
            const float thunder = world->GetRawThunderLevel();

            using P = GameEventS2CPacket;
            if (prevRain != rain) {
                BroadcastToDimension(sessions, level.Dimension(), P::kRainLevelChange, rain);
            }
            if (prevThunder != thunder) {
                BroadcastToDimension(sessions, level.Dimension(), P::kThunderLevelChange, thunder);
            }
            if (wasRaining != world->IsRaining()) {
                // MC broadcasts these to EVERY player; a player elsewhere
                // would only write them into a level that cannot show them
                // (or, sharing this weather, one about to receive the same),
                // so the dimension's players are the ones told.
                BroadcastToDimension(sessions, level.Dimension(),
                                     wasRaining ? P::kStopRaining : P::kStartRaining, 0.0f);
                BroadcastToDimension(sessions, level.Dimension(), P::kRainLevelChange, rain);
                BroadcastToDimension(sessions, level.Dimension(), P::kThunderLevelChange, thunder);
            }
        });

        SyncPlayers(server);
    }

    void ServerWeather::SyncPlayers(IntegratedServer& server) {
        // MC PlayerList.sendLevelInfo's weather: a player placed in a level
        // (the join, a respawn, a dimension change) is sent that level's
        // weather when it is raining there. The client builds a new, dry
        // level on each dimension change (ClientWeather::Reset), so this runs
        // whenever a connection's dimension is not the one it was last
        // brought up to date with.
        PlayerSessionManager* sessions = server.GetSessionManager();
        if (!sessions) { m_synced.clear(); return; }
        std::unordered_set<uint32_t> present;
        for (const auto& session : sessions->GetAllSessions()) {
            if (!session || !session->GetConnection() || !session->GetPlayer()) continue;
            const uint32_t id = session->GetConnectionId();
            present.insert(id);
            const Game::DimensionId dim = Game::DimensionFromRaw(static_cast<int8_t>(session->GetDimensionId()));
            const auto it = m_synced.find(id);
            if (it != m_synced.end() && it->second == dim) continue;
            m_synced[id] = dim;
            const ServerLevel* level = server.GetLevel(WeatherSourceOf(dim));
            const Game::World* world = level ? level->World() : nullptr;
            if (!world || !world->IsRaining()) continue;
            using P = GameEventS2CPacket;
            SendEvent(session->GetConnection(), P::kStartRaining, 0.0f);
            SendEvent(session->GetConnection(), P::kRainLevelChange, world->GetRainLevel(1.0f));
            SendEvent(session->GetConnection(), P::kThunderLevelChange, world->GetThunderLevel(1.0f));
        }
        for (auto it = m_synced.begin(); it != m_synced.end();) {
            if (present.count(it->first) == 0) it = m_synced.erase(it);
            else ++it;
        }
    }

    glm::ivec3 ServerWeather::FindLightningTargetAround(ServerLevel& level, const glm::ivec3& pos) {
        Game::World& world = *level.World();
        const glm::ivec3 center = HeightmapPos(world, Game::HeightmapType::MotionBlocking, pos);

        // MC findLightningRod: the closest LIGHTNING_ROD POI within 128
        // blocks that is its column's top block (WORLD_SURFACE height - 1 —
        // the engine's GetSurfaceHeight is already the top block), struck
        // one above.
        const auto rod = level.Poi().FindClosest(
            [](Game::PoiType t) { return t == Game::PoiType::LightningRod; },
            center, 128, Game::PoiManager::Occupancy::Any,
            [&world](const glm::ivec3& rodPos) {
                return rodPos.y == world.GetSurfaceHeight(rodPos.x, rodPos.z, Game::HeightmapType::WorldSurface);
            });
        if (rod) return *rod + glm::ivec3(0, 1, 0);

        // AABB.encapsulatingFullBlocks(center, center.atY(maxY + 1))
        // .inflate(3): a living entity there that is alive and sees the sky
        // draws the strike to its block.
        const Game::AABB search = Game::AABB::FromMinMax(
            glm::vec3(static_cast<float>(center.x) - 3.0f, static_cast<float>(center.y) - 3.0f,
                      static_cast<float>(center.z) - 3.0f),
            glm::vec3(static_cast<float>(center.x) + 4.0f, static_cast<float>(Game::World::MAX_Y + 2) + 3.0f,
                      static_cast<float>(center.z) + 4.0f));
        std::vector<Game::LivingEntity*> candidates;
        if (ServerLevelBridge* bridge = level.MobLevel()) {
            std::vector<Game::Entity*> entities;
            bridge->GetEntitiesInBox(search, nullptr, entities);
            for (Game::Entity* e : entities) {
                auto* living = dynamic_cast<Game::LivingEntity*>(e);
                if (!living || !living->IsAlive()) continue;
                const glm::ivec3 bp = living->BlockPosition();
                // Level.canSeeSky: the sky light at full strength.
                if (world.GetBrightness(Game::Lighting::LightLayer::Sky, bp.x, bp.y, bp.z) < 15) continue;
                candidates.push_back(living);
            }
        }
        if (!candidates.empty()) {
            return candidates[static_cast<size_t>(m_random.NextInt(static_cast<int>(candidates.size())))]
                ->BlockPosition();
        }
        glm::ivec3 target = center;
        if (target.y == Game::DimensionMinY(level.Dimension()) - 1) target.y += 2;
        return target;
    }

    void ServerWeather::TickThunder(ServerLevel& level) {
        Game::World* world = level.World();
        ServerLevelBridge* bridge = level.MobLevel();
        if (!world || !bridge || !level.Tickets()) return;
        const bool raining = world->IsRaining();
        if (!raining || !world->IsThundering()) return;   // short-circuits the roll, as MC's &&
        PROFILE_ZONE_N("Weather.TickThunder");

        Game::JavaRandom& random = world->Random() ? *world->Random() : m_random;
        for (const Game::Math::ChunkPos& cp : level.Tickets()->GetRandomTickingChunks()) {
            if (!world->GetLoadedChunk(cp.x, cp.z)) continue;
            // MC ServerLevel.tickThunder.
            if (random.NextInt(100000) != 0) continue;
            const glm::ivec3 pos = FindLightningTargetAround(
                level, world->GetBlockRandomPos(cp.x * 16, 0, cp.z * 16, 15));
            if (!world->IsRainingAt(pos.x, pos.y, pos.z)) continue;

            const Game::DifficultyInstance difficulty = bridge->GetCurrentDifficultyAt(pos);
            const bool isTrap = Game::Rules::GetBool(Game::Rules::Id::SpawnMobs) &&
                                random.NextDouble() < static_cast<double>(difficulty.GetEffectiveDifficulty()) * 0.01 &&
                                !IsLightningRod(world->GetBlock(pos.x, pos.y - 1, pos.z));
            if (isTrap) {
                // EntityTypes.SKELETON_HORSE.create(level, EVENT); setTrap(true);
                // setAge(0); setPos(pos) — the block corner, not the centre.
                std::unique_ptr<Game::Mob> mob = MakeMobForLoad(Game::EntityTypeId::SkeletonHorse, bridge);
                if (auto* horse = dynamic_cast<Game::SkeletonHorse*>(mob.get())) {
                    horse->SetTrap(true);
                    horse->SetAge(0);
                    horse->position = glm::dvec3(pos);
                    horse->oldPosition = horse->position;
                    if (MobManager* mobs = level.Mobs()) mobs->Add(std::move(mob));
                }
            }
            // LightningBolt at Vec3.atBottomCenterOf(pos), visual only for a trap.
            SpawnLightningBolt(level, glm::dvec3(pos.x + 0.5, pos.y, pos.z + 0.5), /*visualOnly=*/isTrap);
        }
    }

} // namespace Server
