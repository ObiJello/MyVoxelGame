// File: src/server/level/ServerLevel.cpp

#include "ServerLevel.hpp"
#include "server/level/AurelithCities.hpp"

#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/ExperienceOrbManager.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/FallingBlockStore.hpp"
#include "server/entity/ServerEntityTracker.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ChunkKeeper.hpp"
#include "server/level/EndDragonFight.hpp"
#include "server/level/SilentWardenBossBars.hpp"
#include "server/level/HushStillness.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/world/MyTerrainGenerator.hpp"
#include "server/world/status/ChunkStatusManager.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"
#include "server/world/tracking/ChunkDeltaBroadcaster.hpp"
#include "server/world/tracking/SectionChangeAccumulator.hpp"

#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"

#include <filesystem>
#include "common/core/Profiling_Tracy.hpp"

namespace Server {

    ServerLevel::ServerLevel(const ServerLevelConfig& config,
                             PlayerSessionManager* sessions,
                             IntegratedServer* server)
        : m_config(config), m_sessions(sessions), m_server(server)
    {
        Log::Info("[ServerLevel] Creating '%s'",
                  std::string(Game::DimensionName(m_config.dimension)).c_str());

        m_world = std::make_unique<Game::World>();

        // Order matters and is not obvious: every one of these must be set
        // BEFORE World::Initialize, which is where the chunk provider — and
        // with it the terrain generator — is constructed from them.
        m_world->SetDimension(m_config.dimension);
        m_world->SetReadOnly(m_config.readOnly);
        m_world->SetSavePath(m_config.savePath);
        if (!m_config.worldPath.empty()) {
            m_world->SetMinecraftWorldPath(m_config.worldPath);
        }
        m_world->SetGenerationSeed(m_config.seed);
        m_world->SetGenerateStructures(m_config.generateStructures);

        // World-type customization is an OVERWORLD concept. "amplified" or a
        // flat preset handed to the nether generator would select the wrong
        // noise router; vanilla has no presets for the other two dimensions.
        if (m_config.dimension == Game::DimensionId::Overworld) {
            m_world->SetWorldGenOptions(m_config.worldType, m_config.flatPreset,
                                        m_config.flatLayers, m_config.singleBiome);
            m_world->SetWorldGenTweaks(m_config.worldgenTweaks);
        }

        m_world->Initialize();

        // AFTER Initialize — that is where the provider is constructed, and
        // the cap lives on the provider rather than on the world config.
        //
        // Three resident chunk caches is the real memory cost of three
        // dimensions, and a Nether nobody is standing in has no business
        // holding as many chunks as the overworld. The overworld keeps the
        // engine default; the others get enough for a view distance of 16 plus
        // headroom (33x33 = 1089 chunks in view).
        if (auto* provider = m_world->GetChunkProvider()) {
            provider->SetMaxLoadedChunks(m_config.maxLoadedChunks);
        }

        m_tickets = std::make_unique<ChunkTicketManager>();
        {
            // Our own save has a data folder per dimension; an imported
            // Minecraft world is read-only and keeps nothing.
            std::filesystem::path dataDir, regionDir;
            std::string reason;
            if (!m_config.savePath.empty()) {
                if (auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason)) {
                    dataDir   = root->DataDir(m_config.dimension);
                    regionDir = root->RegionDir(m_config.dimension);
                }
            }
            m_keeper = std::make_shared<ChunkKeeper>(m_config.dimension, dataDir, regionDir,
                                                     m_config.readOnly, m_tickets.get());
            if (auto* provider = m_world->GetChunkProvider()) {
                std::weak_ptr<ChunkKeeper> weak = m_keeper;
                provider->SetChunkSavedCallback([weak](Game::Math::ChunkPos pos, const Game::Chunk& chunk) {
                    if (auto keeper = weak.lock()) keeper->NoteChunkSaved(pos, ChunkHasRedstone(chunk));
                });
            }
        }
        m_status  = std::make_unique<ChunkStatusManager>();
        m_items   = std::make_unique<ItemEntityManager>();
        m_orbs    = std::make_unique<ExperienceOrbManager>();

        // The bridge is built before the manager because the manager holds a
        // pointer to it, and the bridge needs the manager back for entity
        // queries — hence the explicit SetMobManager rather than a constructor
        // argument.
        m_mobLevel = std::make_unique<ServerLevelBridge>(m_world.get(), m_sessions);
        // Pressure plates, tripwire and hoppers count what stands on them
        // through the same bridge the mobs tick against.
        m_world->SetEntityLevel(m_mobLevel.get());
        m_mobs     = std::make_unique<MobManager>(m_mobLevel.get());
        m_mobLevel->SetMobManager(m_mobs.get());
        // Drops and XP land in THIS dimension — see the setter's note.
        m_mobLevel->SetItemAndOrbManagers(m_items.get(), m_orbs.get());
        m_mobLevel->SetPoiManager(&m_poi);
        m_fallingBlocks = std::make_unique<FallingBlockStore>(m_mobLevel.get(), m_mobs.get());
        m_mobTracker = std::make_unique<ServerEntityTracker>();

        // After the three managers, because it reads all of them.
        m_entityStore = std::make_unique<LevelEntityStore>(*this);

        m_changes = std::make_unique<SectionChangeAccumulator>();
        m_deltas  = std::make_unique<ChunkDeltaBroadcaster>(m_server, m_changes.get(),
                                                            m_sessions,
                                                            m_config.dimension);

        // MC ServerLevel: `this.dragonFight = new EndDragonFight(...)` for the
        // End only. After the managers — the fight reads all of them — and
        // handed to the bridge so the dragon and the crystals reach it through
        // EntityLevel::DragonFight().
        if (m_config.dimension == Game::DimensionId::End) {
            m_dragonFight = std::make_unique<EndDragonFight>(*this, m_sessions);
            m_mobLevel->SetDragonFight(m_dragonFight.get());
        }
        // The Silent Warden's boss bar, every dimension (see the header).
        m_wardenBossBars = std::make_unique<SilentWardenBossBars>(*this, m_sessions);
        // The Hush's stillness (see the header). After the bridge, whose
        // flag it drives.
        if (m_config.dimension == Game::DimensionId::Hush) {
            m_stillness = std::make_unique<HushStillness>(*this, m_sessions);
            // Aurelith's cities (AurelithCities.hpp): after the bridge and
            // the managers (the Unsung is raised through them); its SavedData
            // beside the keeper's, in this dimension's data folder.
            std::filesystem::path aurelithData;
            std::string aurelithReason;
            if (!m_config.savePath.empty() && !m_config.readOnly) {
                if (auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, aurelithReason)) {
                    aurelithData = root->DataDir(m_config.dimension);
                }
            }
            m_aurelith = std::make_unique<AurelithCities>(*this, m_sessions, aurelithData);
        }

        worldSpawn = glm::vec3(0.5f, 67.0f, 0.5f);
    }

    ServerLevel::~ServerLevel() {
        PROFILE_ZONE_N("Server.Level.Destroy");
        // Reverse construction order. The broadcaster reads the accumulator on
        // flush, and the mob manager reads the bridge, so both must go first.
        // The fight before the managers it reads — and detached from the
        // bridge first, so no dragon mid-destruction can reach a dead fight.
        { PROFILE_ZONE_N("Server.Level.Managers");
        if (m_mobLevel) m_mobLevel->SetDragonFight(nullptr);
        m_dragonFight.reset();
        if (m_wardenBossBars) m_wardenBossBars->RemoveAll();
        m_wardenBossBars.reset();
        // Before the bridge it writes to.
        if (m_stillness) m_stillness->RemoveAll();
        m_stillness.reset();
        if (m_aurelith) { m_aurelith->Save(); m_aurelith->RemoveAll(); }
        m_aurelith.reset();
        m_deltas.reset();
        m_changes.reset();
        m_mobTracker.reset();
        // Before the managers it reads.
        m_entityStore.reset();
        m_fallingBlocks.reset();
        m_mobs.reset();
        m_mobLevel.reset();
        m_orbs.reset();
        m_items.reset();
        if (m_status)  m_status->Clear();
        if (m_tickets) m_tickets->Clear();
        m_status.reset();
        m_tickets.reset();
        }
        if (m_world) {
            PROFILE_ZONE_N("Server.Level.WorldShutdown");
            m_world->RequestStop();
            m_world->Shutdown();
        }
        { PROFILE_ZONE_N("Server.Level.WorldDestroy");
        m_world.reset();
        }
    }

    bool ServerLevel::InitializeChunkProvider() {
        if (!m_world) return false;
        return m_world->InitializeChunkProvider();
    }

    std::shared_ptr<Game::Chunk> ServerLevel::GetChunkBlocking(Game::Math::ChunkPos pos,
                                                               int ticketTicks) {
        ASSERT_SERVER_THREAD();
        if (!m_world) return nullptr;

        // Ticket first, as PortalTravel::EnsureExitAreaLoaded does: nothing
        // may unload between the load and whatever the caller does with it.
        if (m_tickets && ticketTicks > 0) {
            m_tickets->AddTemporaryTicket(pos, ChunkTicketManager::ENTITY_TICKING_LEVEL, ticketTicks);
        }

        // Resident already: no disk, no generator, no re-entry into the
        // terrain library.
        if (auto resident = m_world->GetLoadedChunk(pos.x, pos.z)) return resident;

        // A generator that is shutting down never completes a request, and
        // ServerChunkCache::getChunk has no timeout of its own.
        if (Game::MyTerrainGenerator* gen = TerrainGenerator(); gen && gen->IsAbortRequested()) {
            return nullptr;
        }

        // The blocking load: ChunkProvider::GetChunk → disk, else
        // MyTerrainGenerator::GenerateChunk → ServerChunkCache::getChunk's
        // managedBlock loop. CompleteChunkLoad puts it in the cache, so
        // World::IsChunkLoaded answers true from here on.
        return m_world->GetChunk(pos.x, pos.z);
    }

    Game::MyTerrainGenerator* ServerLevel::TerrainGenerator() const {
        if (!m_world) return nullptr;
        Game::ChunkProvider* provider = m_world->GetChunkProvider();
        if (!provider) return nullptr;
        return dynamic_cast<Game::MyTerrainGenerator*>(provider->GetGenerator());
    }

    // MC ServerLevel.getSeaLevel, which comes from the dimension's
    // NoiseGeneratorSettings. Read by the natural spawner's water/underground
    // category split, which is why the Nether's 32 matters: with 63 the
    // spawner treats the whole lava sea as "above sea level".
    int ServerLevel::SeaLevel() const {
        switch (m_config.dimension) {
            case Game::DimensionId::Nether:    return 32;
            case Game::DimensionId::End:       return 0;
            case Game::DimensionId::Hush:      return 50;   // MyTerrainGenerator's hush settings
            case Game::DimensionId::TwilightForest: return 0;    // twilight_noise_gen.json sea_level
            case Game::DimensionId::Aether:    return -64;  // skylands.json sea_level (no sea)
            case Game::DimensionId::Overworld: return 63;
        }
    }

    bool ServerLevel::HasWork(const PlayerSessionManager& sessions) const {
        // A level with nobody in it still holds whatever forced tickets it was
        // given (the overworld's spawn chunks), but it costs no simulation.
        // That is what keeps an unvisited Nether free.
        //
        // Standing in it OR looking into it: a dimension seen through a
        // portal must keep simulating, or the far side is a frozen diorama.
        if (sessions.AnySessionLoadsDimension(m_config.dimension)) return true;
        return m_config.dimension == Game::DimensionId::Overworld;
    }

} // namespace Server
