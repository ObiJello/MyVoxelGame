// File: src/server/IntegratedServer.cpp
#include "common/world/level/ModDimensions.hpp"
#include "IntegratedServer.hpp"
#include "server/items/HushItems.hpp"
#include "server/entity/FallingBlockStore.hpp"
#include <cctype>
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/ArmorStand.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/core/SaveVersion.hpp"
#include "server/world/storage/anvil/WorldFolder.hpp"
#include "server/world/storage/anvil/WorldSidecar.hpp"
#include "server/world/storage/anvil/PlayerDataStore.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "commands/TeleportCommand.hpp"
#include "commands/KickCommand.hpp"
#include "commands/GameModeCommand.hpp"
#include "commands/DifficultyCommand.hpp"
#include "commands/KillCommand.hpp"
#include "commands/HealCommand.hpp"
#include "commands/LootCommand.hpp"
#include "commands/EffectCommand.hpp"
#include "level/LevelEntityStore.hpp"   // MakeMobForLoad (the effect mob factory)
#include "common/world/spawn/StructureSpawnOverrides.hpp"
#include "server/world/storage/anvil/SpawnerNbt.hpp"
#include "commands/EntityStatsCommand.hpp"
#include "commands/ShapeCommand.hpp"

#include <deque>
#include <array>
#include <cmath>
#include "commands/SummonCommand.hpp"
#include "commands/ForceLoadCommand.hpp"
#include "commands/SetBlockCommand.hpp"
#include "level/ChunkKeeper.hpp"
#include "level/LighthouseGuide.hpp"
#include "commands/ReplaceAllCommand.hpp"
#include "commands/ExecuteCommand.hpp"
#include "commands/WorldOptionsCommand.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/portal/PortalShape.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/block/RedstonePlus.hpp"
#include "common/world/level/GameRules.hpp"
#include "commands/SpawnAllCommand.hpp"
#include "commands/SheepEatCommand.hpp"
#include "commands/TimeCommand.hpp"
#include "commands/DimensionCommand.hpp"
#include "commands/LocateCommand.hpp"
#include "commands/GameRuleCommand.hpp"
#include "commands/SeedCommand.hpp"
#include "commands/UpdateBlocksCommand.hpp"
#include "commands/InvisibleCommand.hpp"
#include "commands/MorphCommand.hpp"
#include "commands/ControlCommand.hpp"
#include "control/RemoteControlManager.hpp"
#include "entity/MorphCarry.hpp"
#include "entity/MorphBlockAnchor.hpp"
#include "sound/ServerSoundBroadcaster.hpp"
#include "common/sound/SoundEvents.hpp"   // player attack sounds (Player.attack)
#include "common/entity/Morph.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "commands/TickCommand.hpp"
#include "commands/StillnessCommand.hpp"
#include "commands/AurelithCommand.hpp"
#include "level/AurelithCities.hpp"
#include "network/NetworkServer.hpp"
#include "network/ServerConnection.hpp"
#include "network/SendScheduler.hpp"
#include "session/PlayerSessionManager.hpp"
#include "session/PlayerSession.hpp"
#include "player/ServerPlayer.hpp"
#include "level/PlayerSpawnFinder.hpp"
#include "world/ticketing/ChunkTicketManager.hpp"
#include "common/core/Assert.hpp"   // ASSERT_SERVER_THREAD in GetOrCreateLevel
#include "level/ServerLevel.hpp"
#include "level/EndDragonFight.hpp"
#include "level/SilentWardenBossBars.hpp"
#include "level/HushStillness.hpp"
#include "level/PortalTravel.hpp"
#include "world/status/ChunkStatusManager.hpp"
#include "world/tracking/SectionChangeAccumulator.hpp"
#include "world/tracking/ChunkDeltaBroadcaster.hpp"
#include "entity/ItemEntityManager.hpp"
#include "entity/ExperienceOrbManager.hpp"
#include "entity/MobManager.hpp"
#include "entity/LocalMobCapCalculator.hpp"
#include "entity/ServerLevelBridge.hpp"
#include "entity/ServerEntityTracker.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/SpawnEggs.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/ShulkerBullet.hpp"
#include "common/entity/projectile/LlamaSpit.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/projectile/EvokerFangs.hpp"
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/projectile/EyeOfEnder.hpp"
#include "common/entity/LightningBolt.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/mobs/SulfurCube.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/spawn/NaturalSpawner.hpp"
#include "common/world/spawn/GeneratedMobSpawns.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"
#include "common/world/spawn/PotentialCalculator.hpp"
#include "common/physics/Physics.hpp"
#include "common/physics/RayCast.hpp"
#include <limits>
#include "common/core/Mth.hpp"
#include "common/world/pathfinder/PathTypeTable.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/PacketTypes.hpp"

#include "common/core/Log.hpp"
#include "common/core/ThreadPriority.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/TickParallel.hpp"
#include <future>
#include "common/world/level/World.hpp"
#include "common/world/lighting/LevelLightManager.hpp"
#include "client/entity/Player.hpp"
#include "world/ServerWorkerPool.hpp"
#include "world/MyTerrainGenerator.hpp"
#include "world/storage/SectionDataUnpacker.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN
#include "portal/PortalRegistry.hpp"
#endif
#if ENABLE_IMMERSIVE_PORTALS
#include "portal/ImmersivePortalRegistry.hpp"
#include "portal/NetherPortalGeneration.hpp"
#include "portal/EntityPortalTravel.hpp"
#include "commands/PortalCommand.hpp"
#include "common/world/portal/ImmersiveFrame.hpp"
#include "common/world/portal/PortalState.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <thread>

namespace Server {

    namespace {
        // MC uses String.equalsIgnoreCase for the singleplayer-owner test.
        // ASCII-only on purpose: pass unsigned char to tolower — a negative
        // signed char is undefined there.
        bool EqualsIgnoreCaseAscii(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        }

        // ChestBlockEntity's user counter — MC ContainerOpenersCounter
        // .getEntitiesWithContainerOpen, which the lid's 5-tick recheck
        // recounts from: every player whose chest menu covers `pos` (either
        // half of a double) and every copper golem working that chest. Runs
        // on the server thread inside the level's scheduled-tick drain.
        int CountChestUsers(Game::ILevelWrite& level, const glm::ivec3& pos);
    } // namespace

    // Global instance
    std::unique_ptr<IntegratedServer> g_integratedServer = nullptr;

    namespace {
        int CountChestUsers(Game::ILevelWrite& level, const glm::ivec3& pos) {
            IntegratedServer* server = g_integratedServer.get();
            if (!server) return 0;
            const auto* world = dynamic_cast<const Game::World*>(&level);
            if (!world) return 0;

            int count = 0;
            if (PlayerSessionManager* sessions = server->GetSessionManager()) {
                for (const auto& session : sessions->GetAllSessions()) {
                    if (session && session->HasChestOpenAt(world, pos)) ++count;
                }
            }

            // Golems: MC searches maxInteractionRange + 4 around the chest; a
            // golem works a chest from beside it, so 8 blocks covers it.
            ServerLevel* serverLevel = server->GetLevel(level.GetDimension());
            if (serverLevel && serverLevel->World() == world && serverLevel->MobLevel()) {
                const glm::vec3 centre = glm::vec3(pos) + glm::vec3(0.5f);
                const auto box = Game::AABB::FromMinMax(centre - glm::vec3(8.0f),
                                                        centre + glm::vec3(8.0f));
                std::vector<Game::Entity*> nearby;
                serverLevel->MobLevel()->GetEntitiesInBox(box, nullptr, nearby);
                for (Game::Entity* entity : nearby) {
                    auto* golem = dynamic_cast<Game::CopperGolem*>(entity);
                    if (golem && golem->IsAlive() && golem->OpenedChestPos() == pos) ++count;
                }
            }
            return count;
        }
    } // namespace
    
    // Server thread ID for assertions
    std::thread::id g_serverThreadId;

    IntegratedServer::IntegratedServer(const IntegratedServerConfig& config)
        : m_config(config) {
        m_tickDuration = std::chrono::duration<float>(1.0f / static_cast<float>(m_config.tickRate));
        Log::Info("IntegratedServer created with %d TPS", m_config.tickRate);
    }

    IntegratedServer::~IntegratedServer() {
#if ENABLE_IMMERSIVE_PORTALS
        Game::Portals::SetImmersiveFrameLitHandler(nullptr);
#endif
        Game::ChestBlockEntity::SetUserCounter(nullptr);
        if (m_running.load()) {
            Stop();
        }
        // The levels before any member they point into. Members die in
        // reverse declaration order, and m_levels is declared BEFORE the
        // session manager, sound sink and send scheduler — so left to the
        // implicit order, every ~ServerLevel would run after the session
        // manager it holds a pointer to is gone. Shutdown() has normally
        // emptied the array already; this covers a server that is destroyed
        // without it (one whose Start failed, or a failed Initialize).
        for (auto& level : m_levels) {
            level.reset();
        }
        // The sink is a process global; never leave it naming a dead
        // broadcaster (it is cleared in CleanupSessionSystem, which a server
        // that never started does not reach).
        if (m_soundBroadcaster && Game::Sound::GetServerSink() == m_soundBroadcaster.get()) {
            Game::Sound::SetServerSink(nullptr);
        }
        Log::Info("IntegratedServer destroyed");
    }

    // ========================================================================
    // DIMENSIONS
    // ========================================================================

    ServerLevel* IntegratedServer::GetLevel(Game::DimensionId dimension) const {
        return m_levels[static_cast<size_t>(Game::DimensionSlot(dimension))].get();
    }

    ServerLevel& IntegratedServer::Overworld() const {
        // Not a null check: the overworld is built in Initialize and every
        // caller runs after it. A crash here means Initialize failed and the
        // server should never have started, which Start() already refuses.
        return *m_levels[static_cast<size_t>(
            Game::DimensionSlot(Game::DimensionId::Overworld))];
    }

    NetherPortalIndex& IntegratedServer::TwilightPortalIndex(Game::DimensionId dimension) {
        std::unique_ptr<NetherPortalIndex>& slot =
            m_twilightPortalIndexes[static_cast<size_t>(Game::DimensionSlot(dimension))];
        if (!slot) slot = std::make_unique<NetherPortalIndex>(Game::BlockID::TwilightPortal);
        return *slot;
    }

    namespace {

        // The gamerules a World carries, level.dat → World.
        void ApplyLevelDatRules(Game::World& w, const Game::Anvil::LevelDatData& d) {
            w.SetDoDaylightCycle(d.doDaylightCycle);
            w.SetDoMobSpawning(d.doMobSpawning);
            w.SetDoMobGriefing(d.mobGriefing);
            w.SetRandomTickSpeed(d.randomTickSpeed);
            w.SetTntExplodes(d.tntExplodes);
            w.SetDoEntityDrops(d.doEntityDrops);
            w.SetTntExplosionDropDecay(d.tntExplosionDropDecay);
            w.SetBlockExplosionDropDecay(d.blockExplosionDropDecay);
            w.SetMobExplosionDropDecay(d.mobExplosionDropDecay);
        }
        // ...and World → World, for a level built after the overworld.
        void CopyGameRules(const Game::World& from, Game::World& to) {
            to.SetDoDaylightCycle(from.GetDoDaylightCycle());
            to.SetDoMobSpawning(from.GetDoMobSpawning());
            to.SetDoMobGriefing(from.GetDoMobGriefing());
            to.SetRandomTickSpeed(from.GetRandomTickSpeed());
            to.SetTntExplodes(from.GetTntExplodes());
            to.SetDoEntityDrops(from.GetDoEntityDrops());
            to.SetTntExplosionDropDecay(from.GetTntExplosionDropDecay());
            to.SetBlockExplosionDropDecay(from.GetBlockExplosionDropDecay());
            to.SetMobExplosionDropDecay(from.GetMobExplosionDropDecay());
        }
        // ...and World → the game-rule registry (Game::Rules), so the nine
        // World-field rules read the same from both. The World is the
        // authority for them (worlds.json can override advance_time at open,
        // for one), hence this direction.
        void SyncRegistryFromWorld(const Game::World& w) {
            using Game::Rules::Id;
            Game::Rules::SetBool(Id::AdvanceTime,             w.GetDoDaylightCycle());
            Game::Rules::SetBool(Id::SpawnMobs,               w.GetDoMobSpawning());
            Game::Rules::SetBool(Id::MobGriefing,             w.GetDoMobGriefing());
            Game::Rules::Set    (Id::RandomTickSpeed,         w.GetRandomTickSpeed());
            Game::Rules::SetBool(Id::TntExplodes,             w.GetTntExplodes());
            Game::Rules::SetBool(Id::EntityDrops,             w.GetDoEntityDrops());
            Game::Rules::SetBool(Id::TntExplosionDropDecay,   w.GetTntExplosionDropDecay());
            Game::Rules::SetBool(Id::BlockExplosionDropDecay, w.GetBlockExplosionDropDecay());
            Game::Rules::SetBool(Id::MobExplosionDropDecay,   w.GetMobExplosionDropDecay());
        }

    } // namespace

    ServerLevel* IntegratedServer::GetOrCreateLevel(Game::DimensionId dimension) {
        const size_t slot = static_cast<size_t>(Game::DimensionSlot(dimension));
        if (m_levels[slot]) return m_levels[slot].get();

        ASSERT_SERVER_THREAD();

        ServerLevelConfig cfg;
        cfg.dimension          = dimension;
        cfg.readOnly           = m_config.readOnlyWorld;
        cfg.seed               = Overworld().World()->GetGenerationSeed();
        cfg.generateStructures = true;

        // The imported Minecraft world is read by MinecraftChunkLoaderImpl,
        // which knows only one folder and reads <folder>/region — so it gets
        // the dimension's own folder (DIM-1 / DIM1 beside the overworld). An
        // empty path means this world has no Anvil storage at all, in which
        // case the sub-folder would be meaningless — keep it empty so the
        // level is generate-only rather than reading a stray relative path.
        const auto withSubdir = [dimension](std::string base) {
            const std::string_view subdir = Game::DimensionSaveSubdir(dimension);
            if (base.empty() || subdir.empty()) return base;
            if (base.back() != '/' && base.back() != '\\') base += '/';
            base += subdir;
            return base;
        };

        cfg.worldPath = withSubdir(m_config.minecraftWorldPath);   // imported, read-only

        // Our own save is addressed through SaveRoot, whose Dimension(dim)
        // adds DIM-1 / DIM1 itself — the config carries the world root, the
        // same value the overworld gets. Appending the sub-folder here as well
        // used to put the nether at DIM-1/DIM-1/region, which Minecraft never
        // looks at (EnsureDirectories lifts such folders back on open).
        cfg.savePath = m_config.savePath;                          // ours, writable

        // A dimension nobody is standing in still holds a resident chunk cache.
        // Sizing the Nether and the End like the Overworld would triple the
        // engine's chunk memory for two worlds a session may visit for a
        // minute; 1024 chunks is comfortably more than a portal's surroundings.
        cfg.maxLoadedChunks = (dimension == Game::DimensionId::Overworld) ? 5120 : 2048;

        auto level = std::make_unique<ServerLevel>(cfg, m_sessionManager.get(), this);
        if (!level->InitializeChunkProvider()) {
            Log::Error("[IntegratedServer] Could not build a chunk provider for '%s' — "
                       "that dimension is unreachable this session",
                       std::string(Game::DimensionName(dimension)).c_str());
            return nullptr;
        }

        Log::Info("[IntegratedServer] Dimension '%s' is now live",
                  std::string(Game::DimensionName(dimension)).c_str());
        if (level->World()) {
            level->World()->SetDifficulty(static_cast<Game::Difficulty>(m_config.difficulty));
            // Gamerules are server-wide in MC (one GameRules on the server);
            // each World here keeps its own copy, so a new level takes the
            // overworld's current values, including anything /gamerule
            // changed this session.
            CopyGameRules(*Overworld().World(), *level->World());
            // MC DimensionType.fixedTime: a pinned clock. The Hush is a
            // perpetual midnight — its sky, its surface spawning and its
            // monsters' burn check all read this.
            if (const auto fixedTime = Game::DimensionFixedTime(dimension)) {
                level->World()->SetFixedDayTime(*fixedTime);
            }
        }
        m_levels[slot] = std::move(level);
#if ENABLE_IMMERSIVE_PORTALS
        EnsureGlobalPortals(dimension);
#endif
        return m_levels[slot].get();
    }

    ServerLevel& IntegratedServer::LevelOf(const PlayerSession& session) {
        ServerLevel* level = GetLevel(Game::DimensionFromRaw(session.GetDimensionId()));
        // A session whose dimension has been torn down (or was never built)
        // falls back to the overworld rather than dereferencing null. That is
        // a bug upstream, but stranding the player in a world that exists
        // beats crashing the server thread.
        return level ? *level : Overworld();
    }

    Game::World* IntegratedServer::GetWorld() const {
        ServerLevel* level = GetLevel(Game::DimensionId::Overworld);
        return level ? level->World() : nullptr;
    }

    int64_t IntegratedServer::GetBiomeZoomSeed() const {
        const Game::World* world = GetWorld();
        return world ? world->GetBiomeZoomSeed() : 0;
    }

    SectionChangeAccumulator* IntegratedServer::GetChangeAccumulator() const {
        ServerLevel* level = GetLevel(Game::DimensionId::Overworld);
        return level ? level->Changes() : nullptr;
    }

    SectionChangeAccumulator* IntegratedServer::GetChangeAccumulatorFor(
            const Game::World& world) const {
        // Resolved from the World's own dimension rather than by pointer
        // identity so a caller holding a reference (World::SetBlock) needs no
        // back-pointer. The identity check afterwards is what keeps the
        // CLIENT's predicted world — which is also a Game::World, and also
        // claims a dimension — from accumulating into the server's set.
        ServerLevel* level = GetLevel(world.GetDimension());
        if (!level || level->World() != &world) return nullptr;
        return level->Changes();
    }

    ItemEntityManager* IntegratedServer::GetItemEntities() const {
        ServerLevel* level = GetLevel(Game::DimensionId::Overworld);
        return level ? level->Items() : nullptr;
    }

    ExperienceOrbManager* IntegratedServer::GetXpOrbs() const {
        ServerLevel* level = GetLevel(Game::DimensionId::Overworld);
        return level ? level->Orbs() : nullptr;
    }

    ChunkStatusManager* IntegratedServer::GetStatusManager() const {
        ServerLevel* level = GetLevel(Game::DimensionId::Overworld);
        return level ? level->Status() : nullptr;
    }

    MobManager* IntegratedServer::GetMobs() const {
        ServerLevel* level = GetLevel(Game::DimensionId::Overworld);
        return level ? level->Mobs() : nullptr;
    }

    size_t IntegratedServer::GetPendingChunkLoadCount() const {
        size_t total = 0;
        for (const auto& level : m_levels) {
            if (level) total += level->pendingChunkLoads.size();
        }
        return total;
    }

    bool IntegratedServer::Initialize() {
        PROFILE_ZONE_N("Server.Initialize");
        Log::Info("IntegratedServer::Initialize - Creating world on server thread");

        // serverViewDistance stays at default (32) for integrated server — no cap on client.
        // A dedicated server would set this from its config file.
        Log::Info("Server view distance cap: %d chunks, simulation distance cap: %d chunks",
                  m_config.serverViewDistance, m_config.maxSimulationDistance);

        // The session system comes FIRST now, because a ServerLevel needs the
        // PlayerSessionManager to build its ServerLevelBridge. Nothing in
        // InitializeSessionSystem touches a level any more — the pieces that
        // did (ticket manager, accumulator, mob system) moved into
        // ServerLevel, which is exactly why the order could be flipped.
        { PROFILE_ZONE_N("Server.Init.Sessions");
        InitializeSessionSystem();
        }

        // The overworld. The Nether and the End are built on first visit; see
        // GetOrCreateLevel.

        // Lay down the world folder before any level exists, so the very first
        // chunk save has somewhere to go and Minecraft can list the world even
        // if the session ends before a single chunk is written. Idempotent for
        // an existing world apart from rewriting level.dat, which is what
        // vanilla does on every autosave anyway.
        if (!m_config.savePath.empty() && !m_config.readOnlyWorld) {
            std::string reason;
            if (auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason)) {
                // Take session.lock BEFORE writing anything. Two processes
                // interleaving sector allocations in one region file corrupt
                // it in a way nothing downstream can recover from, so a world
                // already open elsewhere is refused rather than shared.
                // The folder is normally created by the Create World screen,
                // but a worlds.json-only entry (pre-folder worlds, dev
                // harness) reaches here with nothing on disk — and the lock
                // below cannot be created inside a folder that does not exist,
                // which silently turned saving off for the whole session.
                {
                    std::string dirError;
                    if (!Game::Anvil::EnsureDirectories(*root, dirError)) {
                        Log::Error("Could not create the world folder: %s", dirError.c_str());
                    }
                }
                std::string lockError;
                m_sessionLock = Game::Anvil::SessionLock::Acquire(*root, lockError);
                if (!m_sessionLock) {
                    // Continue WITHOUT saving rather than refusing to start.
                    // Failing startup here would drop the player back out with
                    // no explanation; this way they get a playable session and
                    // the world another process owns is left untouched.
                    Log::Error("Cannot save this world: %s", lockError.c_str());
                    Log::Error("Playing without saving — close the other window and rejoin to keep changes.");
                    m_config.savePath.clear();
                } else {
                    // An existing world's level.dat is the authority for its
                    // gamerules (MC PrimaryLevelData): read it BEFORE the
                    // rewrite below, which would otherwise replace them with
                    // this launch's defaults. worlds.json only ever knew
                    // advance_time, and only as of the last clean exit.
                    {
                        // The registry is process-wide and the previous
                        // world of this session may have changed it: every
                        // rule back to its default, then the file's values.
                        Game::Rules::ResetAll();
                        Game::Anvil::LevelDatData saved;
                        std::string readError;
                        std::error_code ec;
                        if (std::filesystem::exists(root->LevelDat(), ec) &&
                            Game::Anvil::ReadLevelDat(root->LevelDat(), saved, readError)) {
                            m_savedLevelDat = saved;
                            // The immersive_portals rule is per world and
                            // level.dat carries it (obeycraft.immersive_portals);
                            // it used to be written but never read back, so a
                            // world left with vanilla portals reopened
                            // immersive — with the purple blocks still in
                            // the frames. A --vanilla-portals launch keeps
                            // its say.
                            if (m_config.immersivePortals) m_config.immersivePortals = saved.immersivePortals;
                            m_config.redstonePlus = saved.redstonePlus;
                            m_config.redstoneChunks = saved.redstoneChunks;
                            m_config.veinMineMaxBlocks = saved.veinMineMaxBlocks;
                            m_config.sharedVitals = saved.sharedVitals;
                            m_config.twilightForestEnabled = saved.twilightForestEnabled;
                            m_config.aetherEnabled = saved.aetherEnabled;
                            m_guestCommandAccess.store(saved.guestCommandAccess);
                            for (const auto& [key, value] : saved.gameRules) {
                                if (const Game::Rules::Def* def = Game::Rules::Find(key)) Game::Rules::Set(def->id, value);
                            }
                            m_config.doDaylightCycle = saved.doDaylightCycle;
                            // World Options state the file owns (MC
                            // PrimaryLevelData): cheats, hardcore, the
                            // difficulty lock.
                            m_allowCommands.store(saved.allowCommands);
                            m_hardcore.store(saved.hardcore);
                            m_difficultyLocked.store(saved.difficultyLocked);
                        } else if (!readError.empty()) {
                            Log::Warning("[Anvil] %s — gamerules start from defaults", readError.c_str());
                        }
                    }
                    // Start from what the file already says (rules, spawn) so
                    // the rewrite keeps them; the launch settings go on top.
                    Game::Anvil::LevelDatData meta = m_savedLevelDat.value_or(Game::Anvil::LevelDatData{});
                    if (!m_savedLevelDat) {
                        // A brand-new world: hardcore comes from the create
                        // screen; cheats default on (LevelDatData).
                        m_hardcore.store(m_config.hardcore);
                        meta.hardcore = m_config.hardcore;
                    }
                    meta.allowCommands    = m_allowCommands.load();
                    meta.difficultyLocked = m_difficultyLocked.load();
                    meta.levelName       = m_config.worldDisplayName;
                    meta.gameType        = m_config.defaultGameMode;
                    meta.dayTime         = m_config.initialDayTime;
                    meta.doDaylightCycle = m_config.doDaylightCycle;
                    meta.immersivePortals = m_config.immersivePortals;
                    meta.worldWrapSize    = m_config.worldWrapSize;
                    meta.dimensionStack   = m_config.dimensionStack;
                    meta.redstonePlus     = m_config.redstonePlus;
                    meta.redstoneChunks   = m_config.redstoneChunks;
                    meta.veinMineMaxBlocks = m_config.veinMineMaxBlocks;
                    meta.sharedVitals     = m_config.sharedVitals;
                    meta.twilightForestEnabled = m_config.twilightForestEnabled;
                    meta.aetherEnabled    = m_config.aetherEnabled;
        meta.guestCommandAccess = m_guestCommandAccess.load();
        meta.immersivePortals = m_config.immersivePortals;
        meta.worldWrapSize    = m_config.worldWrapSize;
        meta.dimensionStack   = m_config.dimensionStack;
        meta.redstonePlus     = m_config.redstonePlus;
        meta.redstoneChunks   = m_config.redstoneChunks;
        meta.veinMineMaxBlocks = m_config.veinMineMaxBlocks;
        meta.sharedVitals     = m_config.sharedVitals;
        meta.twilightForestEnabled = m_config.twilightForestEnabled;
        meta.aetherEnabled    = m_config.aetherEnabled;
        meta.guestCommandAccess = m_guestCommandAccess.load();
        meta.difficulty       = m_config.difficulty;
                    // The SEED matters more than anything else here:
                    // Minecraft generates the chunks beyond ours with it, so a
                    // wrong seed means terrain that does not line up at the
                    // border of what we saved. Rewritten on autosave and
                    // shutdown once spawn and time are settled too.
                    meta.seed            = m_config.worldSeed;
                    std::string error;
                    if (!Game::Anvil::CreateWorldFolder(*root, meta, Game::Save::DataVersion(), error)) {
                        Log::Error("Could not prepare the world folder: %s", error.c_str());
                    }
                }
            } else {
                Log::Error("Refusing to use save path '%s': %s",
                           m_config.savePath.c_str(), reason.c_str());
                m_config.savePath.clear();
            }
        }

        {
            PROFILE_ZONE_N("Server.Init.Overworld");
            ServerLevelConfig cfg;
            cfg.dimension          = Game::DimensionId::Overworld;
            cfg.readOnly           = m_config.readOnlyWorld;
            cfg.worldPath          = m_config.minecraftWorldPath;
            cfg.savePath           = m_config.savePath;   // overworld sits at the world root
            cfg.generateStructures = true;
            cfg.maxLoadedChunks    = 5120;
            // Seed and world-type customization are pushed in by PlatformMain
            // AFTER construction via the World accessors, as they always were;
            // ServerLevelConfig carries them only for the dimensions this
            // class creates on its own.
            m_levels[static_cast<size_t>(
                Game::DimensionSlot(Game::DimensionId::Overworld))] =
                std::make_unique<ServerLevel>(cfg, m_sessionManager.get(), this);
        }
        if (Game::World* overworld = GetWorld()) {
            overworld->SetDifficulty(static_cast<Game::Difficulty>(m_config.difficulty));
        }

#if ENABLE_IMMERSIVE_PORTALS
        // After the overworld exists (the registry reads its save root) and
        // before any session can be created, so the first chunk a client
        // gets already carries its portals.
        { PROFILE_ZONE_N("Server.Init.Portals");
        m_immersivePortals = std::make_unique<ImmersivePortalRegistry>(*this);
        m_immersivePortals->Load();
        LoadAoRegions();
        }
#if ENABLE_PORTAL_GUN
        // Portal-gun surfaces are mirrored from the gun registry, which is
        // not persisted: any saved with the file are orphans from the last
        // session and go before a client can see them.
        {
            std::vector<Game::Immersive::PortalId> stale;
            m_immersivePortals->ForEach([&](const Game::Immersive::Portal& p) {
                if (p.kind == Game::Immersive::PortalKind::PortalGun) stale.push_back(p.id);
            });
            for (auto id : stale) m_immersivePortals->Remove(id);
        }
        // ...then the saved pairs come back, and their surfaces with them.
        Game::Portal::ServerRegistry().Load();
        Game::Portal::ServerRegistry().RebuildImmersive();
#endif
        m_netherPortalGeneration = std::make_unique<NetherPortalGeneration>(*this);
        m_entityTravel           = std::make_unique<EntityPortalTravel>(*this);
        Game::Portals::SetImmersiveNetherPortals(m_config.immersivePortals);
        Game::RedstonePlus::SetEnabled(m_config.redstonePlus);
        if (m_config.redstonePlus) Log::Info("[RedstonePlus] enabled for this world");
        // A world whose immersive portals were parked by the rule and whose
        // setting is back on (the rule, or the world's portal option) gets
        // them back at open. With the rule still off, only the Hush and
        // Aether records come back — an older build parked them with the
        // nether's — and ConvertVanillaFamilyPortals turns them into vanilla
        // portal blocks before the first tick (those two families are always
        // vanilla).
        if (m_immersivePortals->HasStash()) {
            if (m_config.immersivePortals) {
                m_immersivePortals->RestoreStashed();
#if ENABLE_PORTAL_GUN
                Game::Portal::ServerRegistry().RebuildImmersive();
#endif
            } else {
                m_immersivePortals->RestoreStashed(/*ruleExemptOnly=*/true);
            }
        }
        Game::Portals::SetWorldWrapSize(m_config.worldWrapSize);
        Game::Portals::SetDimensionStack(m_config.dimensionStack);
#if ENABLE_IMMERSIVE_PORTALS
        // Every dimension's, not just the Overworld's: the registry is
        // global and needs no level to hold a record, and the seams are
        // two-way — the Overworld's CEILING is the reverse of the End's
        // floor portal, so until the End's globals exist there is no way
        // up out of the Overworld. Creating them lazily with the level
        // meant the End was reachable only after it had been visited.
        for (Game::DimensionId dim : Game::kAllDimensions) EnsureGlobalPortals(dim);
#endif
        Game::Portals::SetImmersiveFrameLitHandler(&IntegratedServer::OnImmersiveFrameLit);
#endif
        Game::ChestBlockEntity::SetUserCounter(&CountChestUsers);

        if (!m_config.minecraftWorldPath.empty()) {
            Log::Info("Server world configured with Minecraft world: %s%s",
                      m_config.minecraftWorldPath.c_str(),
                      m_config.readOnlyWorld ? " (read-only)" : "");
        }
        if (!m_config.savePath.empty()) {
            Log::Info("World persists to: %s", m_config.savePath.c_str());
        }

        // Restore world time + gamerule from world metadata (worlds.json).
        // Overworld only: MC's day/night cycle is a property of the overworld
        // and the other two dimensions have no sky.
        Overworld().World()->SetDayTime(m_config.initialDayTime);
        // The mod-dimension switches are process-wide atomics (common code
        // reads them); seed them from THIS world so a previous world's
        // setting does not carry over.
        Game::ModDimensions::SetEnabled(Game::DimensionId::TwilightForest, m_config.twilightForestEnabled);
        Game::ModDimensions::SetEnabled(Game::DimensionId::Aether, m_config.aetherEnabled);
        Overworld().World()->SetDoDaylightCycle(m_config.doDaylightCycle);
        // The rest of the gamerules, from level.dat when the world had one.
        if (m_savedLevelDat) ApplyLevelDatRules(*Overworld().World(), *m_savedLevelDat);
        SyncRegistryFromWorld(*Overworld().World());
        // The tick state the player left this world in (/tick freeze, /tick
        // rate) — an engine-only setting, so it lives in the sidecar. No
        // client is connected yet; joining players get it through
        // updateJoiningPlayer.
        if (!m_config.savePath.empty()) {
            std::string reason;
            if (auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason)) {
                const Game::Anvil::WorldSidecar sidecar = Game::Anvil::ReadWorldSidecar(root->Root().string());
                if (sidecar.tickRate != m_tickRateManager.tickrate()) m_tickRateManager.setTickRate(sidecar.tickRate);
                if (sidecar.tickFrozen) {
                    m_tickRateManager.setFrozen(true);
                    Log::Info("[Tick] World was left frozen — resuming frozen (/tick unfreeze to run)");
                }
                // World Options → Joinable, per world. Applied to the
                // listener too in case it is already up.
                m_joinable.store(sidecar.joinable);
                if (m_networkServer) m_networkServer->SetAcceptingConnections(sidecar.joinable);
                Game::Portals::SetPortalGunAllowed(sidecar.portalGun);
                if (!sidecar.joinable) Log::Info("[WorldOptions] World was left not joinable — other players cannot join until Joinable is turned on");
            }
        }
        Log::Info("Server world initialized successfully");

        // The ticket manager belongs to a level now, so the spawn tickets that
        // used to be added inside InitializeSessionSystem are added here,
        // where the overworld exists.
        Overworld().Tickets()->AddSpawnTickets(Game::Math::ChunkPos(0, 0), 2);
        m_sessionManager->SetLevelServices(Overworld().Tickets(), Overworld().Status());

        // Create ServerPlayer instance (PlayerSession will be created when player joins)
        glm::vec3 spawnPos(0.0f, 67.0f, 0.0f);
        m_serverPlayer = std::make_unique<ServerPlayer>(1, Server::kDefaultPlayerName);
        m_serverPlayer->setPosition(glm::dvec3(spawnPos));

        // NOTE: PlayerSession is now created by PlayerSessionManager when player joins
        // See OnPlayerJoined() where OnPlayerJoin() is called

        // Reset statistics
        m_stats.Reset();

        Log::Info("IntegratedServer initialized successfully");
        return true;
    }

    bool IntegratedServer::Start() {
        if (m_running.load()) {
            Log::Warning("IntegratedServer already running");
            return false;
        }

        // The overworld is the one level Initialize builds eagerly; if it is
        // missing, Initialize did not run (or failed) and there is nothing to
        // tick. The other two are built on first visit and their absence here
        // is normal.
        if (!GetLevel(Game::DimensionId::Overworld)) {
            Log::Error("Cannot start IntegratedServer without world");
            return false;
        }

        Log::Info("Starting IntegratedServer thread...");

        // Create io_context and NetworkServer
        m_ioContext = std::make_unique<net::io_context>();
        // OBEY_HOST_PORT: listen elsewhere than 25565, so a dev-harness
        // instance (tools/redstone_computer/harness.py) can run beside a
        // game that already holds the default port. The local client
        // follows GetPort(), so nothing else needs to know.
        uint16_t port = 25565;
        if (const char* e = std::getenv("OBEY_HOST_PORT")) {
            const int p = std::atoi(e);
            if (p > 0 && p < 65536) port = static_cast<uint16_t>(p);
        }
        m_networkServer = std::make_unique<NetworkServer>(*m_ioContext, port);

        // Start NetworkServer on all interfaces so remote clients can connect
        if (!m_networkServer->Start("0.0.0.0")) {
            Log::Error("Failed to start NetworkServer on 0.0.0.0:%u", static_cast<unsigned>(port));
            // Nothing runs on the io_context yet; release the half-built
            // listener now (acceptor before its context) so the failed server
            // holds no socket while its session is torn down.
            m_networkServer.reset();
            m_ioContext.reset();
            return false;
        }

        Log::Info("NetworkServer listening on 0.0.0.0:%d", m_networkServer->GetPort());
        // The world's saved Joinable choice (read before the listener existed).
        m_networkServer->SetAcceptingConnections(m_joinable.load());

        // Register server commands (MC: Commands.java constructor)
        TeleportCommand::Register(m_commandDispatcher);
        KickCommand::Register(m_commandDispatcher);
        GameModeCommand::Register(m_commandDispatcher);
        DifficultyCommand::Register(m_commandDispatcher);
        KillCommand::Register(m_commandDispatcher);
        HealCommand::Register(m_commandDispatcher);
        LootCommand::Register(m_commandDispatcher);
        EffectCommand::Register(m_commandDispatcher);
        // OOZING's slimes and INFESTED's silverfish are built by the same
        // factory the spawner and /summon use (MobEffects.cpp cannot see it).
        Game::SetEffectMobFactory(&MakeMobForLoad);
        // The monster spawner's server half (building its mobs from their
        // SpawnData compounds, the spawn egg's id rewrite, the type spawn
        // predicate) — SpawnerBlockEntity lives in common code.
        Game::Anvil::InstallSpawnerServerHooks();
        EntityStatsCommand::Register(m_commandDispatcher);
        ShapeCommand::Register(m_commandDispatcher);
        SummonCommand::Register(m_commandDispatcher);
        SetBlockCommand::Register(m_commandDispatcher);
        ForceLoadCommand::Register(m_commandDispatcher);
        ReplaceAllCommand::Register(m_commandDispatcher);
        ExecuteCommand::Register(m_commandDispatcher);
        WorldOptionsCommand::Register(m_commandDispatcher);
        SpawnAllCommand::Register(m_commandDispatcher);
        SheepEatCommand::Register(m_commandDispatcher);
        TimeCommand::Register(m_commandDispatcher);
        DimensionCommand::Register(m_commandDispatcher);
        LocateCommand::Register(m_commandDispatcher);
        GameRuleCommand::Register(m_commandDispatcher);
        SeedCommand::Register(m_commandDispatcher);
        UpdateBlocksCommand::Register(m_commandDispatcher);
        InvisibleCommand::Register(m_commandDispatcher);
        MorphCommand::Register(m_commandDispatcher);
        ControlCommand::Register(m_commandDispatcher);
#if ENABLE_IMMERSIVE_PORTALS
        PortalCommand::Register(m_commandDispatcher);
#endif
        TickCommand::Register(m_commandDispatcher);
        StillnessCommand::Register(m_commandDispatcher);
        AurelithCommand::Register(m_commandDispatcher);
        Log::Info("Server commands registered");

        // Wire disconnect callback so we broadcast entity removal to other clients
        // MC handles a disconnect on the SERVER thread (the play listener's
        // onDisconnect is scheduled there). This callback fires on whichever
        // thread noticed the close — usually the network I/O thread — and
        // OnPlayerDisconnected rewrites session, player and level state the
        // server thread is using at that moment. A mass disconnect ran it
        // concurrently with the tick for 40 sessions at once; copying a
        // session's watch sets while the tick rehashed them looped until the
        // machine ran out of memory (stress test, 2026-09-23). So: queue it,
        // and let the next tick handle it. Once the server thread has
        // stopped (shutdown), it runs directly — Stop() drains the queue.
        m_networkServer->SetOnDisconnection([this](std::shared_ptr<ServerConnection> conn) {
            if (m_serverThreadActive.load() && std::this_thread::get_id() != g_serverThreadId) {
                std::lock_guard<std::mutex> lock(m_closedMutex);
                m_closedConnections.push_back(std::move(conn));
                return;
            }
            OnPlayerDisconnected(conn);
        });

        // Create work guard to keep io_context alive
        m_ioWorkGuard = std::make_unique<WorkGuard>(net::make_work_guard(*m_ioContext));
        
        // Start dedicated network I/O thread (Minecraft-style Netty pattern)
        m_networkThread = std::make_unique<std::thread>([this]() {
            Log::Info("Server network I/O thread started (tid: %zu)", 
                      std::hash<std::thread::id>{}(std::this_thread::get_id()));
            try {
                m_ioContext->run();
                Log::Info("Server network I/O thread exiting normally");
            } catch (const std::exception& e) {
                Log::Error("Server network I/O thread exception: %s", e.what());
            }
        });
        
        Log::Info("✓ Server network I/O thread started");

        m_shouldStop.store(false);
        m_running.store(true);
        m_everStarted = true;

        // Start server thread
        m_serverThread = std::make_unique<std::thread>([this]() { ServerLoop(); });

        Log::Info("IntegratedServer started successfully");
        return true;
    }

    void IntegratedServer::Stop() {
        if (!m_running.load()) {
            return;
        }

        Log::Info("Stopping IntegratedServer...");
        
        // Signal the server thread to stop
        m_shouldStop.store(true);

        // Tell EVERY world to abort any long-running chunk loading loops. A
        // worker parked in the Nether's blocking getChunk is just as capable
        // of holding up the join below as one parked in the Overworld's.
        ForEachLevel([](ServerLevel& level) {
            if (level.World()) level.World()->RequestStop();
            // The tick loop parks on a generator's pipeline queue until its
            // next deadline; without this the join below waits out that
            // sleep — up to a whole tick — on every world exit.
            if (auto* gen = level.TerrainGenerator()) gen->WakePipeline();
        });

        // IMPORTANT: Wait for server thread to finish BEFORE destroying resources.
        // World::RequestStop() signals the terrain library's abort flag, so blocking
        // getChunk() loops will exit promptly.
        if (m_serverThread && m_serverThread->joinable()) {
            // Waits out whatever tick is running (up to a whole 50 ms one).
            PROFILE_ZONE_N("Server.Stop.JoinThread");
            Log::Debug("Waiting for server thread to finish...");
            m_serverThread->join();
            Log::Debug("Server thread finished");
        }
        m_serverThread.reset();

        // Stop the network I/O thread
        if (m_ioContext && m_networkThread) {
            PROFILE_ZONE_N("Server.Stop.NetIO");
            Log::Info("Stopping server network I/O thread...");
            
            // Reset work guard to allow io_context to exit
            m_ioWorkGuard.reset();
            
            // Stop the io_context (this will cause run() to return)
            m_ioContext->stop();
            
            // Wait for the I/O thread to finish
            if (m_networkThread->joinable()) {
                m_networkThread->join();
            }
            m_networkThread.reset();
            
            Log::Info("✓ Server network I/O thread stopped");
        }
        
        // Disconnects queued after the server thread's last tick: both
        // threads are stopped, so they are handled here, on this one.
        DrainClosedConnections();

        // Cleanup session system BEFORE destroying network resources.
        // Sessions and SendScheduler hold ServerConnection shared_ptrs with
        // Asio strands — these must be destroyed while io_context is still alive.
        { PROFILE_ZONE_N("Server.Stop.Sessions");
        CleanupSessionSystem();
        }

        // Now it's safe to destroy NetworkServer and io_context
        // since both the server thread and I/O thread are no longer running
        if (m_networkServer) {
            PROFILE_ZONE_N("Server.Stop.NetworkServer");
            m_networkServer->Stop();
            m_networkServer.reset();
        }

        if (m_ioContext) {
            m_ioContext.reset();
        }

        m_running.store(false);
        Log::Info("IntegratedServer stopped");
    }

    void IntegratedServer::AutoSave() {
        ASSERT_SERVER_THREAD();
        if (m_config.savePath.empty() || m_config.readOnlyWorld) return;

        const auto started = std::chrono::steady_clock::now();

        // Queue every dirty chunk in every dimension. Non-blocking: the
        // storage thread drains it while the game keeps ticking, and the
        // per-chunk cooldown inside AnvilChunkStorage caps how often any one
        // chunk can be rewritten.
        ForEachLevel([](ServerLevel& level) {
            // Background: snapshot here, compress + write on the IO thread.
            if (level.World()) level.World()->SaveAllChunks(/*wait=*/false);
            // Entities are NOT dirty-tracked: a mob walking changes a chunk's
            // entity list without touching a block, so every occupied chunk is
            // reconsidered. O(entities), not O(loaded chunks).
            if (level.Entities()) level.Entities()->SaveAllLoaded();
            if (level.DragonFightController()) level.DragonFightController()->Save();
            if (level.Keeper()) level.Keeper()->Save();
            if (level.Aurelith()) level.Aurelith()->Save();
        });

        SaveAllPlayers();
        WriteLevelDat();
        SaveImmersivePortals();

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        Log::Info("[Anvil] autosave queued in %lld ms", static_cast<long long>(ms));
    }

    void IntegratedServer::SavePlayerData(const ServerPlayer& player) {
        if (m_config.savePath.empty() || m_config.readOnlyWorld) return;
        std::string reason;
        auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason);
        if (!root) return;

        std::string error;
        if (!Game::Anvil::WritePlayerData(*root, player, Game::Save::DataVersion(), error)) {
            Log::Error("Could not save player '%s': %s", player.getName().c_str(), error.c_str());
        }
    }

    bool IntegratedServer::LoadPlayerData(ServerPlayer& player) {
        if (m_config.savePath.empty()) return false;
        std::string reason;
        auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason);
        if (!root) return false;

        std::string error;
        if (!Game::Anvil::ReadPlayerData(*root, player, error)) {
            // A real read failure, not a first join. Say so rather than
            // silently handing the player a fresh inventory.
            if (!error.empty()) {
                Log::Error("Could not load player '%s': %s", player.getName().c_str(), error.c_str());
            }
            return false;
        }
        return true;
    }

    void IntegratedServer::PersistTickState() {
        if (m_config.savePath.empty() || m_config.readOnlyWorld) return;
        std::string reason;
        auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason);
        if (!root) return;
        const std::string dir = root->Root().string();
        Game::Anvil::WorldSidecar sidecar = Game::Anvil::ReadWorldSidecar(dir);
        const bool  frozen = m_tickRateManager.isFrozen();
        const float rate   = m_tickRateManager.tickrate();
        if (sidecar.tickFrozen == frozen && sidecar.tickRate == rate) return;
        sidecar.tickFrozen = frozen;
        sidecar.tickRate   = rate;
        if (!Game::Anvil::WriteWorldSidecar(dir, sidecar)) {
            Log::Warning("[Tick] Could not save the tick state to %s", dir.c_str());
        }
    }

    void IntegratedServer::SaveAllPlayers() {
        if (!m_sessionManager) return;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (session && session->GetPlayer()) SavePlayerData(*session->GetPlayer());
        }
    }

    void IntegratedServer::SetWorldAllowCommands(bool allow) {
        if (m_allowCommands.exchange(allow) == allow) return;
        WriteLevelDat();
    }

    void IntegratedServer::SetGuestCommandAccess(bool access) {
        if (m_guestCommandAccess.exchange(access) == access) return;
        WriteLevelDat();
    }

    void IntegratedServer::SetDifficultyLocked(bool locked) {
        if (m_difficultyLocked.exchange(locked) == locked) return;
        WriteLevelDat();
    }

    // MC IntegratedServer.updateGameModeBasedOnPermission (via
    // setWorldGameType / setForceGameMode): with Force Game Mode on, every
    // GUEST online is moved to the world's default game mode right away; the
    // host keeps theirs.
    void IntegratedServer::EnforceGameModeForGuests() {
        if (!m_forceGameMode.load() || !m_sessionManager) return;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (!session || !session->GetPlayer() || !session->GetConnection()) continue;
            if (session->GetConnection()->IsSingleplayerOwner()) continue;
            ServerPlayer& player = *session->GetPlayer();
            const auto wanted = static_cast<GameMode>(m_config.defaultGameMode);
            if (player.getGameMode() == wanted) continue;
            player.setGameMode(wanted);
            session->GetConnection()->SendPlayerAbilities(player);
        }
    }

    void IntegratedServer::SetWorldGameType(int gameMode) {
        gameMode = std::clamp(gameMode, 0, 3);
        if (m_config.defaultGameMode != gameMode) {
            m_config.defaultGameMode = gameMode;
            WriteLevelDat();
        }
        EnforceGameModeForGuests();
    }

    void IntegratedServer::SetForceGameMode(bool force) {
        m_forceGameMode.store(force);
        EnforceGameModeForGuests();
    }

    uint16_t IntegratedServer::GetPort() const {
        return m_networkServer ? m_networkServer->GetPort() : 0;
    }

    void IntegratedServer::DisconnectGuests(const std::string& reason) {
        if (!m_networkServer) return;
        for (const auto& conn : m_networkServer->GetConnections()) {
            if (!conn || conn->IsSingleplayerOwner()) continue;
            conn->SendDisconnect(reason);
        }
    }

    void IntegratedServer::PersistJoinable() {
        if (m_config.savePath.empty() || m_config.readOnlyWorld) return;
        std::string reason;
        auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason);
        if (!root) return;
        const std::string dir = root->Root().string();
        Game::Anvil::WorldSidecar sidecar = Game::Anvil::ReadWorldSidecar(dir);
        if (sidecar.joinable == m_joinable.load()) return;
        sidecar.joinable = m_joinable.load();
        if (!Game::Anvil::WriteWorldSidecar(dir, sidecar)) {
            Log::Warning("[WorldOptions] Could not save the Joinable setting to %s", dir.c_str());
        }
    }

    bool IntegratedServer::SetJoinable(bool joinable) {
        if (!m_networkServer) return false;
        m_joinable.store(joinable);
        m_networkServer->SetAcceptingConnections(joinable);
        PersistJoinable();
        if (!joinable) {
            // MC IntegratedServer.unpublishServer: "multiplayer.disconnect.server_shutdown".
            DisconnectGuests("Server closed");
            Log::Info("[IntegratedServer] World closed to other players (was on port %d)", m_networkServer->GetPort());
        } else {
            Log::Info("[IntegratedServer] World open to other players on port %d", m_networkServer->GetPort());
        }
        return true;
    }

    bool IntegratedServer::ChangePort(uint16_t port) {
        if (!m_networkServer) return false;
        if (port == m_networkServer->GetPort()) return true;
        if (!m_networkServer->Rebind(port)) return false;
        DisconnectGuests("Server closed");
        return true;
    }

    void IntegratedServer::SetDifficulty(int difficulty) {
        ASSERT_SERVER_THREAD();
        m_config.difficulty = std::clamp(difficulty, 0, 3);
        ForEachLevel([&](ServerLevel& level) {
            if (level.World()) level.World()->SetDifficulty(static_cast<Game::Difficulty>(m_config.difficulty));
        });
        WriteLevelDat();
    }

    void IntegratedServer::WriteLevelDat() {
        if (m_config.savePath.empty() || m_config.readOnlyWorld) return;
        if (!m_levels[0] || !m_levels[0]->World()) return;

        std::string reason;
        auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason);
        if (!root) return;

        Game::Anvil::LevelDatData meta;
        meta.levelName       = m_config.worldDisplayName;
        meta.gameType        = m_config.defaultGameMode;
        meta.hardcore        = m_hardcore.load();
        meta.allowCommands   = m_allowCommands.load();
        meta.difficultyLocked = m_difficultyLocked.load();
        meta.seed            = m_levels[0]->World()->GetGenerationSeed();
        // Gamerules from the LIVE overworld World, not the launch config:
        // /gamerule writes the World, and this file is what brings the
        // change back next session.
        const Game::World& ow = *m_levels[0]->World();
        meta.dayTime                 = ow.GetDayTime();
        meta.doDaylightCycle         = ow.GetDoDaylightCycle();
        meta.doMobSpawning           = ow.GetDoMobSpawning();
        meta.mobGriefing             = ow.GetDoMobGriefing();
        meta.randomTickSpeed         = ow.GetRandomTickSpeed();
        meta.tntExplodes             = ow.GetTntExplodes();
        meta.doEntityDrops           = ow.GetDoEntityDrops();
        meta.tntExplosionDropDecay   = ow.GetTntExplosionDropDecay();
        meta.blockExplosionDropDecay = ow.GetBlockExplosionDropDecay();
        meta.mobExplosionDropDecay   = ow.GetMobExplosionDropDecay();
        // The rest of the registry — every vanilla rule, so the file is one
        // Minecraft reads back whole.
        SyncRegistryFromWorld(ow);
        meta.gameRules.clear();
        for (size_t i = 0; i < Game::Rules::kCount; ++i) {
            const Game::Rules::Def& def = Game::Rules::AllDefs()[i];
            meta.gameRules[def.key] = Game::Rules::GetInt(def.id);
        }
        meta.immersivePortals = m_config.immersivePortals;
        meta.worldWrapSize    = m_config.worldWrapSize;
        meta.dimensionStack   = m_config.dimensionStack;
        meta.redstonePlus     = m_config.redstonePlus;
        meta.redstoneChunks   = m_config.redstoneChunks;
        meta.veinMineMaxBlocks = m_config.veinMineMaxBlocks;
        meta.sharedVitals     = m_config.sharedVitals;
        meta.twilightForestEnabled = m_config.twilightForestEnabled;
        meta.aetherEnabled    = m_config.aetherEnabled;
        meta.guestCommandAccess = m_guestCommandAccess.load();
        meta.difficulty       = m_config.difficulty;
        meta.spawnX          = static_cast<int>(std::floor(m_worldSpawn.x));
        meta.spawnY          = static_cast<int>(std::floor(m_worldSpawn.y));
        meta.spawnZ          = static_cast<int>(std::floor(m_worldSpawn.z));

        std::string error;
        if (!Game::Anvil::WriteLevelDat(*root, meta, Game::Save::DataVersion(), error)) {
            Log::Error("Could not write level.dat: %s", error.c_str());
        }
    }

    void IntegratedServer::Shutdown() {
        Stop();
        
        // Log final statistics
        LogStats();
        
        // Save unimplemented blocks report
        auto& tracker = Game::UnimplementedBlockTracker::GetInstance();
        if (tracker.GetUniqueBlockCount() > 0) {
            Log::Info("Saving unimplemented blocks report...");
            tracker.SaveToFile();
        }
        
        // Clear state
        ForEachLevel([](ServerLevel& level) { level.pendingChunkLoads.clear(); });

        // Note: CleanupSessionSystem() already called in Stop() before io_context destruction

        // Save every dimension, then destroy the levels.
        //
        // Only the SAVE is done here: ~ServerLevel already stops and shuts down
        // its world (and tears the rest of the bundle down in reverse
        // construction order), so calling Shutdown here as well would double it.
        //
        // A server that never started (Start failed — its port was taken) has
        // nothing to save, and its spawn is still the constructor's
        // placeholder: rewriting level.dat would move an existing world's
        // spawn to (0, 67, 0).
        if (m_everStarted) {
        ForEachLevel([](ServerLevel& level) {
            if (!level.World()) return;
            Log::Info("Saving server-owned world '%s'...",
                      std::string(Game::DimensionName(level.Dimension())).c_str());
            { PROFILE_ZONE_N("Server.Shutdown.SaveChunks");   level.World()->SaveAllChunks(); }
            { PROFILE_ZONE_N("Server.Shutdown.SaveEntities");
              // Flush: entity writes go through a background I/O worker; at
              // shutdown every one of them must be on disk before the region
              // files close.
              // Every entity read still outstanding lands first (MC saveAll):
              // a chunk whose read is Pending cannot be saved.
              if (level.Entities()) {
                  level.Entities()->CompleteLoads();
                  level.Entities()->SaveAllLoaded();
                  level.Entities()->Flush();
              } }
            if (level.DragonFightController()) level.DragonFightController()->Save();
            if (level.Keeper()) level.Keeper()->Save();
            if (level.Aurelith()) level.Aurelith()->Save();
        });
        // Rewrite level.dat now that the values it describes are settled. At
        // Initialize the seed had not been pushed in yet and the world spawn
        // had not been searched for, so the file written then carries
        // placeholders. Vanilla rewrites it on every autosave and on shutdown
        // for the same reason.
        { PROFILE_ZONE_N("Server.Shutdown.LevelDat");
        WriteLevelDat();
        SaveImmersivePortals();
        }
        }

        // Release the session lock LAST, once everything is durable — while it
        // is held, no other process can be writing this world.
        m_sessionLock.reset();

        { PROFILE_ZONE_N("Server.Shutdown.DestroyLevels");
        for (auto& level : m_levels) {
            level.reset();
        }
        }

        Log::Info("IntegratedServer shutdown complete");
    }

    void IntegratedServer::SetPlayer(Game::ClientPlayer* player) {
        m_player = player;
        // Session position will be set when the first move packet arrives
    }

    std::shared_ptr<PlayerSession> IntegratedServer::GetPlayerSession() const {
        // Delegate to SessionManager (migrated from direct m_playerSession member)
        if (m_sessionManager) {
            return m_sessionManager->GetSession(1); // playerId=1 for integrated server
        }
        return nullptr;
    }

    Game::Math::ChunkPos IntegratedServer::GetPlayerChunkPosition() const {
        auto session = GetPlayerSession();
        if (session) return session->GetChunkPosition();
        return {0, 0};
    }

    // ── F3 debug sample ──────────────────────────────────────────────────

    IntegratedServer::DebugSample IntegratedServer::GetDebugSample() const {
        std::lock_guard<std::mutex> lock(m_debugSampleMutex);
        return m_debugSample;
    }

    void IntegratedServer::DrainTickTimeSamples(std::vector<std::array<int64_t, 4>>& out) {
        std::lock_guard<std::mutex> lock(m_debugSampleMutex);
        out.insert(out.end(), m_tickTimeSamples.begin(), m_tickTimeSamples.end());
        m_tickTimeSamples.clear();
    }

    void IntegratedServer::SampleDebugInfo(int64_t tickExecNanos, int64_t timeBetweenTicksNanos) {
        DebugSample d;
        d.valid = true;
        d.smoothedTickMs = static_cast<float>(m_tickRateManager.averageTickTimeNanos()) / 1.0e6f;
        d.msPerTick = m_tickRateManager.millisecondsPerTick();
        d.frozen = m_tickRateManager.isFrozen();
        d.stepping = m_tickRateManager.isSteppingForward();
        d.sprinting = m_tickRateManager.isSprinting();

        auto session = GetPlayerSession();
        ServerLevel* level = nullptr;
        glm::vec3 pos(0.0f);
        if (session) {
            d.dimension = Game::DimensionFromRaw(session->GetDimensionId());
            pos = session->GetPosition();
            level = GetLevel(d.dimension);
        }
        if (level) {
            if (Game::World* world = level->World()) {
                d.loadedChunks = world->GetLoadedChunkCount();
                d.difficulty = static_cast<int>(world->GetDifficulty());
                d.dayTime = world->GetDayTime();
                d.gameTime = world->GetGameTime();
                const int bx = static_cast<int>(std::floor(pos.x));
                const int by = static_cast<int>(std::floor(pos.y));
                const int bz = static_cast<int>(std::floor(pos.z));
                const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(bx, bz);
                if (auto chunk = world->GetChunk(cpos.x, cpos.z)) {
                    d.chunkLoaded = true;
                    d.heightmapsPrimed = chunk->AreHeightmapsPrimed();
                    if (d.heightmapsPrimed) {
                        const int lx = bx - cpos.x * Game::Math::CHUNK_SIZE_X;
                        const int lz = bz - cpos.z * Game::Math::CHUNK_SIZE_Z;
                        d.heightWorldSurface = chunk->GetSurfaceHeight(lx, lz, Game::HeightmapType::WorldSurface);
                        d.heightOceanFloor = chunk->GetSurfaceHeight(lx, lz, Game::HeightmapType::OceanFloor);
                        d.heightMotionBlocking = chunk->GetSurfaceHeight(lx, lz, Game::HeightmapType::MotionBlocking);
                        d.heightMotionBlockingNoLeaves = chunk->GetSurfaceHeight(lx, lz, Game::HeightmapType::MotionBlockingNoLeaves);
                    }
                    d.biome = world->GetBiome(bx, by, bz);
                    d.skyLight = world->GetBrightness(Game::Lighting::LightLayer::Sky, bx, by, bz);
                    d.blockLight = world->GetBrightness(Game::Lighting::LightLayer::Block, bx, by, bz);
                    d.inhabitedTime = chunk->InhabitedTime();
                }
                if (m_debugWantsChunkGen.load(std::memory_order_relaxed)) {
                    // Re-sampled only when the feet block moves (MC caches on
                    // lastPos too): the debug density functions and a climate sample.
                    const glm::ivec3 feet(bx, by, bz);
                    if (feet != m_debugGenPos || d.dimension != m_debugGenDimension) {
                        m_debugGenPos = feet;
                        m_debugGenDimension = d.dimension;
                        m_debugGenLines.clear();
                        Game::MyTerrainGenerator* gen = level->TerrainGenerator();
                        minecraft::levelgen::RandomState* rs = gen ? gen->GetRandomState() : nullptr;
                        if (rs != nullptr && gen != nullptr) {
                            // MC DebugEntryChunkGeneration.update: one caching
                            // context for the density line and the climate.
                            minecraft::levelgen::density::SamplerContext samplerContext =
                                minecraft::levelgen::density::SamplerContext::builder().enableCaches().build();
                            if (auto* noiseGenerator = dynamic_cast<const minecraft::levelgen::NoiseBasedChunkGenerator*>(
                                    gen->GetChunkGenerator())) {
                                noiseGenerator->addDebugScreenInfo(m_debugGenLines, rs,
                                                                   minecraft::core::BlockPos(bx, by, bz), samplerContext);
                            }
                            if (gen->GetBiomeSource() != nullptr) {
                                const minecraft::world::biome::Climate::Sampler sampler =
                                    rs->createClimateSampler(samplerContext);
                                gen->GetBiomeSource()->addDebugInfo(m_debugGenLines, bx, by, bz, sampler);
                            }
                        }
                    }
                    d.chunkGenLines = m_debugGenLines;
                }
                if (m_debugWantsChunkMap.load(std::memory_order_relaxed)) {
                    // MC IntegratedServer.createChunkLoadStatusView(16 + RADIUS_AROUND_FULL_CHUNK).
                    d.statusRadius = 16 + 3;
                    const int diameter = d.statusRadius * 2 + 1;
                    d.chunkStatus.resize(static_cast<size_t>(diameter) * diameter, 0);
                    ChunkStatusManager* status = level->Status();
                    for (int z = 0; z < diameter; ++z) {
                        for (int x = 0; x < diameter; ++x) {
                            const Game::Math::ChunkPos cp{ cpos.x - d.statusRadius + x, cpos.z - d.statusRadius + z };
                            ChunkStatus st = status ? status->GetChunkStatus(cp) : ChunkStatus::EMPTY;
                            if (st == ChunkStatus::EMPTY && world->IsChunkLoaded(cp.x, cp.z)) st = ChunkStatus::FULL;
                            d.chunkStatus[static_cast<size_t>(z) * diameter + x] = static_cast<uint8_t>(st);
                        }
                    }
                }
            }
            if (ChunkTicketManager* tickets = level->Tickets()) {
                const auto st = tickets->GetStats();
                d.tickets = st.totalTickets;
                d.playerTickets = st.playerTickets;
                d.blockTickingChunks = st.blockTickingChunks;
                d.entityTickingChunks = st.entityTickingChunks;
            }
            if (ChunkKeeper* keeper = level->Keeper()) d.forceLoadedChunks = static_cast<int>(keeper->SourceCount());
            if (MobManager* mobs = level->Mobs()) {
                d.spawnableChunks = mobs->GetSpawnableChunkCount();
                for (int i = 0; i < 8; ++i) {
                    d.categoryCounts[i] = mobs->CountForCategory(i);
                    d.mobCount += d.categoryCounts[i];
                }
            }
        }

        std::lock_guard<std::mutex> lock(m_debugSampleMutex);
        m_debugSample = std::move(d);
        // MC TpsDebugDimensions for the tick that just ENDED THE WAIT: the
        // full tick is the distance between this tick's start and the last
        // one's, the method time is the previous tick's ServerTick, and idle
        // is the remainder. This engine has no scheduled-task phase.
        if (m_prevTickExecNanos > 0 && timeBetweenTicksNanos > 0 && m_tickTimeSamples.size() < 4096) {
            const int64_t idle = std::max<int64_t>(0, timeBetweenTicksNanos - m_prevTickExecNanos);
            m_tickTimeSamples.push_back({ timeBetweenTicksNanos, m_prevTickExecNanos, 0, idle });
        }
        m_prevTickExecNanos = tickExecNanos;
    }

    glm::vec3 IntegratedServer::GetPlayerPosition() const {
        auto session = GetPlayerSession();
        if (session) return session->GetPosition();
        return glm::vec3(0.0f, 67.0f, 0.0f);
    }

    void IntegratedServer::LogStats() const {
        Log::Info("IntegratedServer Statistics:");
        Log::Info("  Ticks Processed: %zu", m_stats.ticksProcessed.load());
        Log::Info("  Chunks Loaded: %zu", m_stats.chunksLoaded.load());
        Log::Info("  Chunks Sent: %zu", m_stats.chunksSent.load());
        Log::Info("  Block Changes: %zu", m_stats.blockChangesProcessed.load());
        Log::Info("  Packets Received: %zu", m_stats.packetsReceived.load());
        Log::Info("  Packets Sent: %zu", m_stats.packetsSent.load());
        Log::Info("  Average Tick Time: %.2fms", m_stats.averageTickTime.load());
        Log::Info("  Average TPS: %.1f", m_stats.averageTPS.load());
    }

    // ========================================================================
    // SERVER THREAD MAIN LOOP
    // ========================================================================

    void IntegratedServer::ServerLoop() {
        PROFILE_THREAD("ServerThread");
        // Has a real deadline (20 TPS = 50ms/tick) but it is not the frame, and
        // it mostly dispatches to the worker pool rather than computing itself.
        Core::SetCurrentThreadPriority(Core::ThreadPriorityClass::Elevated);
        using clock = std::chrono::steady_clock;
        using namespace std::chrono;
        
        // The tick budget is no longer a constant: /tick rate changes it, and
        // /tick sprint drops it to zero. Read from the manager every iteration,
        // exactly as MC's runServer does
        // (MinecraftServer.java:719-790 — `thisTickNanos =
        // this.tickRateManager.nanosecondsPerTick()` inside the loop).
        static constexpr auto SPIN_CUSHION = 2ms;    // Start spinning 2ms before target

        // MC MinecraftServer overload handling (MinecraftServer.java:737-743,
        // constants at :2280). Both numbers are MC's:
        //   OVERLOADED_THRESHOLD_NANOS       = 1s   (20 * 1s / 20)
        //   OVERLOADED_WARNING_INTERVAL_NANOS = 10s
        static constexpr auto OVERLOADED_THRESHOLD = 1s;
        static constexpr auto OVERLOADED_WARNING_INTERVAL = 10s;
        auto lastOverloadWarning = clock::now();

        // Store server thread ID for assertions
        g_serverThreadId = std::this_thread::get_id();
        m_serverThreadActive.store(true);
        
        Log::Info("IntegratedServer main loop started (Target: %d TPS, ThreadId: %zu)",
                  m_config.tickRate, std::hash<std::thread::id>{}(g_serverThreadId));

        // Initialize the OVERWORLD's chunk provider on the server thread so
        // ServerChunkCache captures the correct main thread ID (matching
        // Minecraft's architecture where the server thread owns all chunk data
        // structures). The Nether and the End do the same inside
        // GetOrCreateLevel, which is likewise server-thread-only.
        ServerLevel* overworld = GetLevel(Game::DimensionId::Overworld);
        if (overworld && overworld->InitializeChunkProvider()) {
            // World::Initialize no longer does this — with three Worlds the
            // last one constructed would win, so a Nether built later would
            // silently become what every raycast reads.
            //
            // The server no longer claims it AT ALL. Every caller of
            // Raycast::CastRay is client code (block targeting, the
            // third-person camera pull-in) and the client owns the pointer;
            // nothing under src/server raycasts.
            //
            // This used to set it to the overworld "because PlatformMain
            // overwrites it moments later" — which was a RACE, not a
            // sequence. PlatformMain assigns on the main thread while this
            // runs on the server thread, so whichever lost the race decided
            // what the crosshair pointed at, and when this one won, targeting
            // was pinned to the overworld forever: in the Nether the crosshair
            // hit overworld geometry (or nothing at all once those chunks
            // unloaded) and no block could be placed.
        }

        // ── World spawn selection (MC MinecraftServer.setInitialSpawn) ─────
        // A world whose level.dat already names a spawn keeps it — MC runs
        // setInitialSpawn only for a level that is not yet initialized, so an
        // existing world (this engine's own saves after their first run, a
        // generated one like the redstone machines) always reopens at the
        // spawn it was saved with. A new world asks the generator: climate
        // SpawnFinder picks the region, a chunk spiral finds dry land,
        // getBaseHeight supplies the surface Y. Runs before any client can
        // join, so every player — host and remote — spawns (and is teleported
        // on join) to this position.
        //
        // OVERWORLD ONLY, as in MC: setInitialSpawn is a property of the
        // overworld, and the other two dimensions place arrivals from a portal
        // exit or the End platform instead.
        Game::World* spawnWorld = overworld ? overworld->World() : nullptr;
        if (m_savedLevelDat && m_savedLevelDat->hasSpawn && spawnWorld) {
            m_worldSpawn = glm::vec3(static_cast<float>(m_savedLevelDat->spawnX) + 0.5f,
                                     static_cast<float>(m_savedLevelDat->spawnY),
                                     static_cast<float>(m_savedLevelDat->spawnZ) + 0.5f);
            Log::Info("[IntegratedServer] World spawn from level.dat: (%.1f, %.1f, %.1f)",
                      m_worldSpawn.x, m_worldSpawn.y, m_worldSpawn.z);
            overworld->worldSpawn = m_worldSpawn;
            if (m_serverPlayer) m_serverPlayer->setPosition(glm::dvec3(m_worldSpawn));
            if (m_sessionManager) m_sessionManager->SetWorldSpawn(m_worldSpawn);
        } else if (m_config.minecraftWorldPath.empty() && spawnWorld && spawnWorld->GetChunkProvider()) {
            if (auto* generator = spawnWorld->GetChunkProvider()->GetGenerator()) {
                const glm::ivec3 spawnBlock = generator->FindSpawnPosition();
                m_worldSpawn = glm::vec3(static_cast<float>(spawnBlock.x) + 0.5f,
                                         static_cast<float>(spawnBlock.y),
                                         static_cast<float>(spawnBlock.z) + 0.5f);

                // The Y above is a worldgen NOISE ESTIMATE — getBaseHeight over
                // WORLD_SURFACE_WG, which predates surface rules, trees, snow
                // layers and every other decoration placed into the column. Land
                // on it directly and you stand inside whatever was built on top.
                //
                // MC does not use it as a spawn either: setInitialSpawn walks an
                // 11x11 chunk spiral asking PlayerSpawnFinder.getSpawnPosInChunk,
                // which reads REAL blocks. Same spiral here, loading each chunk
                // as we probe it — ChunkProvider::GetChunk generates on demand
                // and blocks, which is this engine's equivalent of the
                // SPAWN_SEARCH ticket MC takes per candidate.
                auto* provider = spawnWorld->GetChunkProvider();
                const int spawnChunkX = static_cast<int>(std::floor(spawnBlock.x / 16.0f));
                const int spawnChunkZ = static_cast<int>(std::floor(spawnBlock.z / 16.0f));
                int xOff = 0, zOff = 0, dx = 0, dz = -1;
                bool resolved = false;
                for (int i = 0; i < 11 * 11 && !resolved; ++i) {
                    if (xOff >= -5 && xOff <= 5 && zOff >= -5 && zOff <= 5) {
                        const Game::Math::ChunkPos probe(spawnChunkX + xOff, spawnChunkZ + zOff);
                        if (provider->GetChunk(probe)) {
                            if (auto found = PlayerSpawnFinder::GetSpawnPosInChunk(*spawnWorld, probe)) {
                                m_worldSpawn = glm::vec3(static_cast<float>(found->x) + 0.5f,
                                                         static_cast<float>(found->y),
                                                         static_cast<float>(found->z) + 0.5f);
                                resolved = true;
                                Log::Info("[IntegratedServer] World spawn resolved against real "
                                          "terrain at (%d, %d, %d) after %d chunk probe(s)",
                                          found->x, found->y, found->z, i + 1);
                            }
                        }
                    }
                    // MC's square-spiral turn rule.
                    if (xOff == zOff || (xOff < 0 && xOff == -zOff) ||
                        (xOff > 0 && xOff == 1 - zOff)) {
                        const int t = dx; dx = -dz; dz = t;
                    }
                    xOff += dx; zOff += dz;
                }
                if (!resolved) {
                    // Nothing standable in 121 chunks (all ocean, say). Push the
                    // estimate out of any geometry rather than spawning inside it
                    // — MC's fixupSpawnHeight is the same last resort.
                    m_worldSpawn = PlayerSpawnFinder::FixupSpawnHeight(*spawnWorld, spawnBlock);
                    Log::Warning("[IntegratedServer] No standable spawn within 5 chunks — "
                                 "height-corrected estimate to (%.1f, %.1f, %.1f)",
                                 m_worldSpawn.x, m_worldSpawn.y, m_worldSpawn.z);
                }
                Log::Info("[IntegratedServer] World spawn set to (%.1f, %.1f, %.1f)",
                          m_worldSpawn.x, m_worldSpawn.y, m_worldSpawn.z);
                // The level carries its own copy; keep the two from drifting.
                // Portal exits read the destination LEVEL's spawn, not this
                // class's, so a stale one there would strand an arrival.
                overworld->worldSpawn = m_worldSpawn;
                // The host ServerPlayer was constructed at the legacy spawn
                // before the world could tell us better; move it now.
                if (m_serverPlayer) {
                    m_serverPlayer->setPosition(glm::dvec3(m_worldSpawn));
                }
                // Session manager drives spawn-chunk tickets + join positions
                // off its config copy — keep it in sync.
                if (m_sessionManager) {
                    m_sessionManager->SetWorldSpawn(m_worldSpawn);
                }
            }
        }

#if ENABLE_IMMERSIVE_PORTALS
        // After the spawn search, for the same reason it runs here: the
        // conversion loads the portals' chunks, and only this thread pumps
        // the generator.
        ConvertVanillaFamilyPortals();
#endif

        m_lastTickTime = clock::now();
        m_lastTickStartTime = m_lastTickTime;
        
        // Absolute scheduling - next tick time
        auto nextTickTime = clock::now() + nanoseconds(m_tickRateManager.nanosecondsPerTick());

        // MC MinecraftServer.delayedTasksMaxNextTickTimeNanos: set after every
        // tick to max(now + tick budget, nextTickTime) — see the pump below.
        clock::time_point delayedTasksUntil = clock::now();

        while (!m_shouldStop.load()) {
            // MC MinecraftServer.runServer: a sprinting tick gets a budget of
            // ZERO and the schedule is re-based to now, so the loop runs flat
            // out; every other tick gets the manager's current budget.
            //
            // `tickRateManager.tick()` recomputes runGameElements and counts a
            // `step` down. MC calls it slightly later — inside tickServer, so
            // its sprint check reads the PREVIOUS tick's runGameElements —
            // which is a one-tick lag with no observable effect, since a sprint
            // force-unfreezes and runGameElements is true throughout either
            // way. Calling it up here keeps both the sprint check and the world
            // gate below reading the same, current, value.
            m_tickRateManager.tick();
            const bool sprinting = m_tickRateManager.isSprinting() &&
                                   m_tickRateManager.checkShouldSprintThisTick();
            if (sprinting) {
                nextTickTime = clock::now();
                lastOverloadWarning = nextTickTime;
            }
            const auto tickBudget = sprinting
                ? nanoseconds(0)
                : nanoseconds(m_tickRateManager.nanosecondsPerTick());

            // ── Overload: skip ahead, do NOT catch up ────────────────────────
            //
            // Direct port of MC MinecraftServer.runServer (:737-743):
            //
            //     long behindTimeNanos = Util.getNanos() - this.nextTickTimeNanos;
            //     if (behindTimeNanos > OVERLOADED_THRESHOLD_NANOS + 20L * thisTickNanos
            //         && this.nextTickTimeNanos - this.lastOverloadWarningNanos
            //            >= OVERLOADED_WARNING_INTERVAL_NANOS + 100L * thisTickNanos) {
            //        long ticks = behindTimeNanos / thisTickNanos;
            //        LOGGER.warn("Can't keep up! ...");
            //        this.nextTickTimeNanos += ticks * thisTickNanos;
            //        this.lastOverloadWarningNanos = this.nextTickTimeNanos;
            //     }
            //
            // The re-base is the important half. Without it the schedule stays
            // in the past after a stall and the loop fires one instant tick per
            // missed slot — a burst of double-speed simulation (mob AI, growth,
            // item despawn all run fast), and it also erases the evidence:
            // UpdateStatistics is a 0.9/0.1 EMA, so ~100 catch-up ticks put the
            // readout back at 20 TPS before the next stats line prints. That is
            // precisely how a multi-second stall lived here unnoticed.
            //
            // Measured against the SCHEDULE, not tick execution time, so it
            // catches a slow tick and a descheduled thread alike.
            if (!sprinting) {
                const auto behind = clock::now() - nextTickTime;
                if (behind > OVERLOADED_THRESHOLD + 20 * tickBudget &&
                    nextTickTime - lastOverloadWarning >=
                        OVERLOADED_WARNING_INTERVAL + 100 * tickBudget) {
                    const auto ticksBehind = behind / tickBudget;
                    Log::Warning("Can't keep up! Is the server overloaded? Running %lld ms "
                                 "or %lld ticks behind",
                                 static_cast<long long>(
                                     duration_cast<milliseconds>(behind).count()),
                                 static_cast<long long>(ticksBehind));
                    nextTickTime += ticksBehind * tickBudget;
                    lastOverloadWarning = nextTickTime;
                }
            }

            // Wait until next tick — but drain the chunk pipeline while waiting
            // instead of sleeping straight through it.
            //
            // ServerWorkers blocked in ServerChunkCache::getChunk are waiting on
            // a queue only this thread may drain. Pumping it once per tick meant
            // every chunk paid ~25 ms (half a 50 ms tick) of pure latency before
            // generation even started — measured as 32.79 ms of waiting inside a
            // 68.68 ms chunk load, i.e. 48% of it.
            //
            // Structure is MC MinecraftServer.waitUntilNextTick (:893):
            //
            //     this.runAllTasks();
            //     this.managedBlock(() -> !this.haveTime());
            //
            // i.e. pump one unit at a time and park ONLY when the pipeline has
            // nothing left, re-checking the deadline before each unit. That
            // replaced a fixed 1 ms cadence around an unbounded pump, which is
            // how a single call could blow through the tick deadline by seconds
            // — the clock was consulted once per call rather than once per unit.
            //
            // The park is MC's waitForTasks: sleep for the WHOLE remaining
            // window and let a worker submitting a task wake us
            // (LockSupport.park/unpark, here a condition variable). A fixed
            // polling interval instead would both burn wakeups on an empty
            // queue and add up to that interval of latency to every chunk
            // handed back — the very latency this pump exists to remove.
            //
            // The park can only listen to ONE generator's queue, and there is
            // one queue PER DIMENSION (MyTerrainGenerator keeps its main-thread
            // executor to itself). So with a second level live, work landing on
            // the other queue cannot wake us — we would sleep out the whole
            // window while a ServerWorker sat blocked in that dimension's
            // getChunk. Cap the park in that case and come back round to pump
            // the others; a 2 ms revisit is far cheaper than a wedged worker.
            static constexpr auto MULTI_LEVEL_PARK_CAP = 2ms;
            // MC haveTime():
            //
            //     now < (mayHaveDelayedTasks ? delayedTasksMaxNextTickTimeNanos
            //                                : nextTickTimeNanos)
            //
            // with delayedTasksMaxNextTickTimeNanos = max(tickEnd + tick
            // budget, nextTickTime), set after every tick. While chunk work is
            // pending, the window after a tick is at least ONE WHOLE TICK long
            // — even when the server is behind and nextTickTime is already in
            // the past. An overloaded MC server therefore trades tick rate for
            // chunk loading; it never starves the chunk pipeline.
            //
            // Stopping at nextTickTime alone (as this loop did) gave a server
            // that was behind NO pipeline time at all: measured 2026-09-23 with
            // 100 bots, generation fell from ~245 to ~9 chunks a second as soon
            // as ticks ran long, with 29k chunks waiting.
            //
            // Once every generator is idle (MC: pollTask() found nothing,
            // mayHaveDelayedTasks = false) the deadline falls back to
            // nextTickTime, so an idle pipeline never delays a tick; work that
            // lands during the park re-opens the extended window, as a task
            // arriving during MC's waitForTasks does.
            const auto pumpUntil = nextTickTime - SPIN_CUSHION;
            const auto delayedUntil = std::max(pumpUntil, delayedTasksUntil - SPIN_CUSHION);
            bool mayHaveDelayedTasks = true;
            for (;;) {
                if (m_shouldStop.load()) break;
                const auto deadline = mayHaveDelayedTasks ? delayedUntil : pumpUntil;
                if (clock::now() >= deadline) break;
                PumpChunkPipeline(deadline);

                // PumpChunkPipeline only returns before the deadline when EVERY
                // generator is idle, so there is genuinely nothing to do until
                // something lands on one of the queues.
                if (clock::now() >= deadline) break;
                mayHaveDelayedTasks = false;
                if (clock::now() < pumpUntil) {
                    // Zoned so a shutdown that catches the thread here shows
                    // as "parked", not as a blank in the trace.
                    PROFILE_ZONE_N("Server.Park");
                    Game::MyTerrainGenerator* parkOn = nullptr;
                    int liveGenerators = 0;
                    ForEachLevel([&](ServerLevel& level) {
                        if (auto* gen = level.TerrainGenerator()) {
                            ++liveGenerators;
                            if (!parkOn) parkOn = gen;
                        }
                    });

                    if (!parkOn) {
                        // No queue to be woken through: sleep in short slices
                        // so a stop request is seen within a couple of ms.
                        std::this_thread::sleep_until(std::min<clock::time_point>(
                            pumpUntil, clock::now() + MULTI_LEVEL_PARK_CAP));
                    } else if (liveGenerators == 1) {
                        parkOn->WaitForPipelineWork(pumpUntil);
                    } else {
                        parkOn->WaitForPipelineWork(std::min<clock::time_point>(
                            pumpUntil, clock::now() + MULTI_LEVEL_PARK_CAP));
                    }
                    // Woken (or a slice ended): pump again, with the extended
                    // window back in force should work have arrived.
                    mayHaveDelayedTasks = true;
                } else {
                    break;   // idle and past nextTickTime: tick now
                }
            }

            // Micro-spin for the final stretch to land exactly on time.
            // A stop request skips it and the tick: the park above already
            // wakes on it, and this spin was what still held Stop's join
            // for the rest of the 50 ms (30-45 ms on every world exit).
            while (!m_shouldStop.load() && clock::now() < nextTickTime) {
                std::this_thread::yield();
            }
            if (m_shouldStop.load()) break;

            auto tickStart = clock::now();

            // MC MinecraftServer.haveTime() is `getNanos() < nextTickTimeNanos`
            // — an ABSOLUTE deadline, so a tick that has already overrun does
            // zero optional work rather than a fixed extra slice of it. Publish
            // it for the phases inside the tick that can bound themselves.
            m_tickDeadline = tickStart + tickBudget;

            try {
                ServerTick();
            }
            catch (const std::exception& e) {
                Log::Error("Server tick failed: %s", e.what());
            }
            catch (...) {
                Log::Error("Server tick failed with unknown exception");
            }

            // Update statistics with both tick execution time and time between ticks
            auto tickEnd = clock::now();
            float tickExecutionTime = duration<float, std::milli>(tickEnd - tickStart).count();
            float timeBetweenTicks = duration<float, std::milli>(tickStart - m_lastTickStartTime).count();
            UpdateStatistics(tickExecutionTime, timeBetweenTicks);
            m_lastTickStartTime = tickStart;
            SampleDebugInfo(duration_cast<nanoseconds>(tickEnd - tickStart).count(),
                            static_cast<int64_t>(timeBetweenTicks * 1.0e6f));


            // Feeds /tick query's P50/P95/P99, and its "lagging" test.
            m_tickRateManager.recordTickTime(
                duration_cast<nanoseconds>(tickEnd - tickStart).count());
            m_stress.EndTick(duration_cast<nanoseconds>(tickEnd - tickStart).count());
            m_stress.MaybeReport(*this);
            if (sprinting) {
                m_tickRateManager.endTickWork();
            }

            PROFILE_FRAME_MARK_NAMED("ServerTick");

            // Advance to next tick using absolute schedule (no drift). A
            // sprinting tick adds nothing, so the next iteration starts
            // immediately.
            nextTickTime += tickBudget;
            // MC: delayedTasksMaxNextTickTimeNanos = max(now + thisTickNanos,
            // nextTickTimeNanos) — the pump window above.
            delayedTasksUntil = std::max(clock::now() + tickBudget, nextTickTime);
        }

        m_serverThreadActive.store(false);
        Log::Info("IntegratedServer main loop ended");
    }

    void IntegratedServer::ServerTick() {
        PROFILE_ZONE;
        // Calculate delta time for this tick
        auto currentTime = std::chrono::steady_clock::now();
        auto deltaTime = std::chrono::duration<float>(currentTime - m_lastTickTime).count();
        m_lastTickTime = currentTime;

        // Track current server tick
        static int64_t serverTick = 0;
        serverTick++;
        m_currentServerTick = serverTick;

        // Network I/O is now handled by dedicated thread (no need to poll)
        // The I/O thread runs continuously and processes all async operations
        m_stress.BeginTick();   // phase marks below: ServerStressStats

        // Players whose connections closed since the last tick (queued by the
        // disconnect callback — see there).
        DrainClosedConnections();
        
        // === 1. DRAIN C2S QUEUES ===
        // CRITICAL: Tick all connections to drain their packet queues
        // This is the Minecraft way - process packets on the server thread
        if (m_networkServer) {
            // Get a snapshot of current connections
            auto connections = m_networkServer->GetConnections();
            
            // Create a separate vector to hold strong references during iteration
            // This prevents connections from being destroyed while we're using them
            std::vector<Server::ServerConnectionPtr> activeConnections;
            activeConnections.reserve(connections.size());
            
            // Filter out null and disconnected connections
            for (auto& conn : connections) {
                if (conn && conn->GetState() != Network::ConnectionState::DISCONNECTED) {
                    activeConnections.push_back(conn);
                }
            }
            
            // Now tick each active connection
            // Even if a connection disconnects itself during tick(), 
            // our shared_ptr in activeConnections keeps it alive
            for (auto& conn : activeConnections) {
                try {
                    conn->tick();  // Drain incoming packets and apply to listeners
                } catch (const std::exception& e) {
                    Log::Warning("Exception during connection tick: %s", e.what());
                }
            }
            
            // activeConnections will be destroyed here, releasing any disconnected connections
        }
        m_stress.Mark(ServerStressStats::Phase::Packets);
        
        // Process the new session management system FIRST
        // This updates watch sets before broadcasting block changes
        if (m_sessionManager) {
            // Tick all player sessions (updates watch sets)
            m_sessionManager->Tick(serverTick);

            // The night skip (MC ServerLevel.tick's sleep block). Gated like
            // the world itself: nobody sleeps through a paused game.
            if (SimulationRuns()) TickSleep();

            // Process expired tickets, per dimension. Tickets are per level
            // because they pin chunks, and a chunk only exists inside one
            // world.
            //
            // Then propagate this tick's ticket changes — the sessions above
            // just moved their players' tickets. MC ServerChunkCache.tick ->
            // runDistanceManagerUpdates, once per level per tick: this is the
            // one point where chunk levels change (ChunkTicketManager.hpp).
            // Every level, not only the ones with work: a level everybody
            // just left still has to drop its levels.
            ForEachLevel([&](ServerLevel& level) {
                ChunkTicketManager* tickets = level.Tickets();
                if (!tickets) return;
                if (level.HasWork(*m_sessionManager)) tickets->ProcessExpiredTickets(serverTick);
                tickets->RunAllUpdates();
            });

            // Process send queues
            if (m_sendScheduler) {
                m_sendScheduler->ProcessSendQueues();
            }
        }
        m_stress.Mark(ServerStressStats::Phase::Sessions);

        // === 2b. PAUSE STATE ===
        //
        // MC IntegratedServer.tickServer:102-110, with the multiplayer rule
        // this engine can actually answer (see IntegratedServer::IsPaused).
        //
        // Computed HERE, after the packet drain and the session tick, so an
        // unpause that arrived this tick takes effect on this tick rather than
        // leaving the world frozen for one more.
        {
            const bool wasPaused = m_paused.load(std::memory_order_relaxed);

            bool paused = true;   // no players == paused, exactly as MC does
            if (m_sessionManager) {
                for (const auto& session : m_sessionManager->GetAllSessions()) {
                    if (!session) continue;
                    if (!session->IsPaused()) { paused = false; break; }
                }
            }

            if (paused != wasPaused) {
                m_paused.store(paused, std::memory_order_relaxed);
                // Every client mirrors the flag (ServerPausedS2C): a player
                // whose menu is open freezes their own body only while the
                // WORLD is paused — vanilla's singleplayer pause — and keeps
                // moving while somebody else is still playing.
                for (const auto& session : m_sessionManager->GetAllSessions()) {
                    if (session && session->GetConnection()) session->GetConnection()->SendServerPaused(paused);
                }

                if (paused && m_shouldStop.load()) {
                    // The pause that shutdown produces: the owner's disconnect
                    // empties the session list one tick before the loop is
                    // asked to stop, and "no players" reads as paused. Saving
                    // here doubled every exit's save — Shutdown() flushes the
                    // same chunks, entities, level.dat and portals moments
                    // later, on a world nothing has touched since.
                } else if (paused) {
                    // MC saves on the way IN, not out: "Saving and pausing
                    // game..." at IntegratedServer.java:107. Pausing is when
                    // the player walked away from the keyboard, which is
                    // exactly when an unexpected power cut costs the most.
                    Log::Info("Saving and pausing game...");
                    PROFILE_ZONE_N("Server.PauseSave");
                    SaveAllPlayers();
                    ForEachLevel([](ServerLevel& level) {
                        // Background, as the autosave (see there).
                        if (level.World()) level.World()->SaveAllChunks(/*wait=*/false);
                        if (level.Entities()) level.Entities()->SaveAllLoaded();
                    });
                    WriteLevelDat();
                    SaveImmersivePortals();
                } else {
                    // MC forceTimeSynchronization() on resume (:117). The
                    // clients kept counting their own day-time while the server
                    // did not, so without this everyone is ahead by however long
                    // the pause lasted.
                    Log::Info("Resuming game");
                    ForceTimeSync();
                }
            }
        }

        // === 3. FLUSH ACCUMULATED BLOCK CHANGES ===
        // This MUST happen AFTER session tick (so watch sets are updated)
        // All block changes from this tick are broadcast to watchers now.
        // One accumulator + broadcaster per level: a SectionPos carries no
        // dimension, so a shared pair would send a Nether edit to whoever is
        // standing on the Overworld chunk with the same x/z.
        if (m_sessionManager) {
            ForEachLevel([&](ServerLevel& level) {
                // Light first (MC ChunkHolder.broadcastChanges): drain the
                // level light engine — every block change since the last tick
                // — and send what it changed, so each client holds this
                // tick's light before it applies this tick's blocks and
                // re-meshes once.
                FlushLightUpdates(level);
                if (!level.HasWork(*m_sessionManager)) return;
                if (level.Deltas()) level.Deltas()->flush();
            });
        }

        // === 3b. ACK CLIENT BLOCK PREDICTIONS ===
        // Strictly AFTER the delta flush. The ack tells each client "you may
        // retire your predictions up to sequence N", and the client resolves a
        // retired prediction against the last block state we sent it. If the
        // ack overtook this tick's block updates, a correct prediction would
        // roll back to the pre-interaction block and then get re-applied when
        // the update landed — a visible flicker on every placement. MC gets
        // the same ordering by flushing the ack at the top of the following
        // tick (ServerGamePacketListenerImpl.tick:284).
        if (m_sessionManager) {
            for (auto& session : m_sessionManager->GetAllSessions()) {
                if (session) session->FlushBlockChangeAck();
            }
        }

        // NOTE: Player entities (host AND remote) are ticked from
        // PlayerSession::Tick via m_sessionManager->Tick() above — the old
        // direct m_serverPlayer->tick() here would double-tick the host.

#if ENABLE_PORTAL_GUN
        // Portal gun: per-tick crossing detection. Runs AFTER player tick so
        // the registry sees post-physics positions. Cheap when no portals
        // are placed (early-outs in PortalRegistry::Tick).
        // Frozen along with the rest of the simulation — teleporting through a
        // portal is a world event, not a networking one.
        if (SimulationRuns()) {
            Game::Portal::ServerRegistry().Tick(this);
        }
#endif
#if ENABLE_IMMERSIVE_PORTALS
        // Nether portal generation (far-side search/build) and the frame
        // integrity sweep. Simulation, so frozen with it.
        if (SimulationRuns() && m_netherPortalGeneration) {
            m_netherPortalGeneration->Tick(serverTick);
        }
#endif
        m_stress.Mark(ServerStressStats::Phase::BlocksLight);

        // === 2. SESSION-DRIVEN CHUNK LOADING (Minecraft-style) ===
        //
        // View distance FIRST: it decides the size of the tracking view the
        // scan below diffs against, and a settings packet that arrives after
        // the join (the render-distance slider, or one that lost the race with
        // session setup) has no other way in — nothing resends it.
        ApplyPendingClientViewDistances();

        // Process watch set changes: request loading for new chunks entering view
        ProcessWatchSetChanges();
        m_stress.Mark(ServerStressStats::Phase::WatchSet);

        // Force-loaded and redstone-indexed chunks: the loads their tickets
        // would have caused in MC (ChunkKeeper.hpp).
        ServiceKeptChunks();

        // Process async chunk load results from ServerWorkerPool
        ProcessAsyncChunkResults();

        // Entities whose entities/*.mca read finished since last tick join
        // their level (MC PersistentEntitySectionManager.tick ->
        // processPendingLoads). Every level, before the simulation below.
        ForEachLevel([](ServerLevel& level) {
            if (level.Entities()) level.Entities()->ProcessPendingLoads();
        });
        m_stress.Mark(ServerStressStats::Phase::ChunkResults);

        // === 3. RUN WORLD SIMULATION (no chunk loading — session system handles that) ===
        //
        // Gated on runsNormally() — MC's `/tick freeze`. Only the SIMULATION
        // stops: packet draining, session ticks, chunk streaming and the delta
        // broadcast above all keep running, so a frozen server stays joinable,
        // keeps sending chunks, and still answers commands (including
        // `/tick unfreeze`). That is exactly the split vanilla makes, and
        // freezing the whole tick instead would lock the player out of the
        // server they just froze.
        if (m_sessionManager && SimulationRuns()) {
            // Once per dimension, but only for the dimensions that have work.
            // HasWork is false for a Nether nobody is standing in, which is
            // what keeps an unvisited dimension off the tick budget entirely
            // rather than costing a full simulation pass forever.
            ForEachLevel([&](ServerLevel& level) {
                if (!level.HasWork(*m_sessionManager)) return;
                Game::World* world = level.World();
                if (!world) return;

                // Hand the world this tick's simulation set before it runs. MC
                // does the equivalent inside ServerChunkCache.tickChunks, which
                // walks ChunkMap's block-ticking chunks; World deliberately
                // can't reach for the ticket manager itself, so the server
                // pushes it in.
                //
                // Recomputed every tick because players move. It is a walk of
                // the ticket manager's level cache, which is already maintained
                // for other reasons — not a fresh distance computation per
                // chunk.
                if (level.Tickets()) {
                    // The lists are cached in the ticket manager and rebuilt
                    // only when a level solve changes them; the world keys its
                    // own set to the same version. With a simulation ring far
                    // beyond the view distance the block-ticking list is tens
                    // of thousands of chunks, and rebuilding a set of them
                    // every tick was part of a 121 ms tick.
                    const uint64_t version = level.Tickets()->LevelsVersion();
                    std::vector<Game::Math::ChunkPos> randomTicking = level.Tickets()->GetRandomTickingChunks();
                    // MC ServerChunkCache.tickChunks: every ticking chunk is a
                    // chunk a player inhabits — bump its InhabitedTime by the
                    // game-time delta (one tick here; frozen ticks never reach
                    // this loop). Over the random-ticking list, i.e. vanilla's
                    // reach: inhabited time feeds local difficulty and mob
                    // spawning, both of which happen near players.
                    //
                    // LOADED chunks only. World::GetChunk loads — and
                    // GENERATES — on a miss, synchronously, dependency
                    // pyramid included; right after a far teleport the
                    // ticking set is a thousand chunks around the new
                    // position that the async loader has not delivered yet,
                    // and this loop generated them one by one inside the
                    // tick: 8.6 s for the first, then 4 s stalls every tick
                    // until the streamer caught up (hang-2026-09-06_21-59-32).
                    // A chunk not resident yet has nobody in it to count.
                    for (const Game::Math::ChunkPos& cp : randomTicking) {
                        if (auto chunk = world->GetLoadedChunk(cp.x, cp.z)) chunk->IncrementInhabitedTime(1);
                    }
                    world->SetBlockTickingChunks(level.Tickets()->GetBlockTickingChunks(),
                                                 std::move(randomTicking), version);
                }
                world->WorldLoop(deltaTime);

                // Dropped items. Inside the runsNormally() gate on purpose:
                // `/tick freeze` should stop items mid-air and stop their
                // despawn clock, exactly like every other piece of world
                // simulation.
                if (level.Items()) {
                    PROFILE_ZONE_N("ItemEntityTick");
                    std::vector<int32_t> removed;
                    std::vector<ItemPickupEvent> pickups;
                    level.Items()->Tick(world, m_sessionManager.get(), removed, pickups);
                    // Pickups first: the take packet is what retires a
                    // collected entity client-side, and it has to arrive while
                    // the client still has the entity to animate.
                    if (!pickups.empty()) {
                        BroadcastItemEntityPickups(level.Dimension(), pickups);
                    }
                    if (!removed.empty()) {
                        BroadcastItemEntityRemovals(level.Dimension(), removed);
                    }
                    BroadcastItemEntityUpdates(level, serverTick);
                }

                // Experience orbs. Same gate and the same
                // pickups-before-removals ordering as items, for the same
                // reasons.
                if (level.Orbs()) {
                    PROFILE_ZONE_N("XpOrbTick");
                    std::vector<int32_t> removed;
                    std::vector<XpOrbPickupEvent> pickups;
                    level.Orbs()->Tick(world, m_sessionManager.get(), removed, pickups);
                    if (!pickups.empty()) {
                        BroadcastXpOrbPickups(level.Dimension(), pickups);
                    }
                    if (!removed.empty()) {
                        BroadcastItemEntityRemovals(level.Dimension(), removed);
                    }
                    BroadcastXpOrbUpdates(level, serverTick);
                }

                // Mobs. Same gate as items — `/tick freeze` must stop AI,
                // physics and the despawn clock together, or an unfrozen mob
                // walks through a frozen world.
                // Streamed /shape build, overworld only, ahead of the mobs
                // so a freshly placed TNT block can be lit the same tick.
                if (&level == &Overworld()) TickShapeJob();

                TickMobs(level, serverTick);

                // Portals. After TickMobs on purpose: that is where the player
                // views are synced from their ServerPlayers and where mobs
                // move, so the contact test runs against this tick's positions
                // rather than last tick's.
                TickPortals(level);
#if ENABLE_IMMERSIVE_PORTALS
                // Immersive surfaces: mobs, items and orbs that pierced one
                // this tick move through it — after the vanilla portal tick
                // for the same reason it follows TickMobs.
                if (m_entityTravel) m_entityTravel->Tick(level, serverTick);
#endif
            });
        }
        else if (m_sessionManager) {
            // /tick freeze — MC's TickRateManager.isEntityFrozen exempts
            // PLAYERS, and the entity tracker keeps running: what a player
            // does to a frozen world still happens and still shows. The
            // player sessions ticked above (attacks, bow charge, item use);
            // here the entities those created are absorbed (a spawn egg's
            // baby, the arrows a bow releases — MC's addFreshEntity is
            // immediate) and every mob's state is synced to the clients.
            // The mobs themselves do not tick: a hit's hurt cooldown does
            // not run down and an arrow does not fly until the thaw — the
            // same as vanilla, where a volley loosed under a freeze all
            // launches together on /tick unfreeze.
            ForEachLevel([&](ServerLevel& level) {
                if (!level.HasWork(*m_sessionManager)) return;
                if (MobManager* mobs = level.Mobs()) mobs->AbsorbSpawned();
                SyncMobsToClients(level, {});
            });
        }
        m_stress.Mark(ServerStressStats::Phase::World);

        // === 4. SEND CHUNKS per player (Minecraft's PlayerChunkSender pattern) ===
        if (m_sessionManager) {
            auto sessions = m_sessionManager->GetAllSessions();
            for (auto& session : sessions) {
                // Each pending chunk names its own dimension — the session
                // may be streaming the far side of a portal alongside the
                // world it stands in.
                std::vector<PlayerSession::PendingChunkRef> notResident;
                session->SendNextChunks(
                    [this](Game::DimensionId dim) -> Game::World* {
                        ServerLevel* level = GetLevel(dim);
                        return level ? level->World() : nullptr;
                    },
                    &notResident);
                for (const auto& ref : notResident) {
                    ServerLevel* L = GetLevel(ref.dimension);
                    if (!L) continue;
                    // Same split as ProcessWatchSetChanges' onEnter: a chunk
                    // that came back into the cache meanwhile only needs to be
                    // queued again (RequestChunkLoad's already-loaded branch
                    // deliberately does not queue it).
                    if (L->World() && L->World()->IsChunkLoaded(ref.pos.x, ref.pos.z)) {
                        if (L->Entities()) L->Entities()->RequestLoad(ref.pos);
                        session->MarkChunkPendingToSend(ref.dimension, ref.pos);
                    } else {
                        RequestChunkLoad(ref.dimension, ref.pos, 0);
                    }
                }
            }
        }
        m_stress.Mark(ServerStressStats::Phase::ChunkSend);

        // === 5. BROADCAST PLAYER POSITIONS to other players (every tick) ===
        // MC sends entity movement every tick and the client interpolates
        // over three; at half rate the lerp never reached its target
        // before the next one landed, and a fast body — a scaled player
        // covers metres a tick — moved in visible lurches.
        if (m_sessionManager) {
            m_sessionManager->BroadcastPlayerPositions();
        }
        // /morph item: who touched whom this tick.
        if (m_morphCarry) m_morphCarry->Tick();
        // /morph block: locks whose block or player is gone.
        if (m_morphBlockAnchor) m_morphBlockAnchor->Tick();
        // /morph snow_golem, Alt held (the move packet's anim byte): snow
        // under the feet — MC SnowGolem.aiStep, the four cells a quarter
        // block out, air that a snow layer can stand on.
        if (m_sessionManager) {
            for (const auto& s : m_sessionManager->GetAllSessions()) {
                if (!s || !s->GetPlayer()) continue;
                const ServerPlayer& p = *s->GetPlayer();
                if (!Game::Morph::IsMob(p.getMorph(), Game::EntityTypeId::SnowGolem)) continue;
                if (serverTick % 20 == 0) {
                    Log::Info("[Morph] golem trail: %s anim=%u", p.getName().c_str(),
                              static_cast<unsigned>(p.getMorphAnim()));
                }
                if (p.getMorphAnim() == 0) continue;
                ServerLevel* level = GetLevel(Game::DimensionFromRaw(static_cast<int8_t>(s->GetDimensionId())));
                Game::World* world = level ? level->World() : nullptr;
                ServerLevelBridge* mobLevel = level ? level->MobLevel() : nullptr;
                if (!world || !mobLevel || !mobLevel->Blocks()) continue;
                // Not behind mobGriefing, unlike the real golem's: this is a
                // player's own act, done on purpose with the key held.
                const glm::dvec3 pos = p.getPosition();
                for (int i = 0; i < 4; ++i) {
                    const int xx = static_cast<int>(std::floor(pos.x + static_cast<double>((i % 2 * 2 - 1) * 0.25f)));
                    const int yy = static_cast<int>(std::floor(pos.y + 1.0e-4));
                    const int zz = static_cast<int>(std::floor(pos.z + static_cast<double>((i / 2 % 2 * 2 - 1) * 0.25f)));
                    const glm::ivec3 snowPos(xx, yy, zz);
                    if (!world->IsChunkLoaded(xx >> 4, zz >> 4)) continue;
                    const Game::BlockID at = world->GetBlock(xx, yy, zz);
                    const bool canStand = Game::CanSurviveAt(*mobLevel->Blocks(), snowPos, Game::BlockID::SnowLayer);
                    if (serverTick % 20 == 0 && i == 0) {
                        Log::Info("[Morph] golem trail: cell (%d,%d,%d) block=%u canStand=%d",
                                  xx, yy, zz, static_cast<unsigned>(at), canStand ? 1 : 0);
                    }
                    if (at != Game::BlockID::Air || !canStand) continue;
                    const bool placed = world->SetBlock(xx, yy, zz, Game::BlockID::SnowLayer, Game::World::UpdateFlags::All);
                    if (!placed && serverTick % 20 == 0) Log::Info("[Morph] golem trail: SetBlock refused");
                }
            }
        }

        // === 5b. TIME SYNC every 20 ticks (MC MinecraftServer.tickChildren) ===
        if (serverTick % 20 == 0) {
            ForceTimeSync();
        }
        m_stress.Mark(ServerStressStats::Phase::Broadcast);

        // === 6. CLEANUP: unload chunks with no watchers ===
        // Every tick, in slices (MC ChunkMap.processUnloads) — see there.
        UnloadUnwatchedChunks();

        // Terrain library holder unloading — MC ServerChunkCache.tick:
        // purgeStaleTickets, the distance-manager pass that turns removed
        // tickets into unload candidates, then ChunkMap.processUnloads while
        // there is time, forced only past 2,000 ready to unload (MC).
        // Every tick, as MC. Without it every chunk the library ever touched
        // stayed resident: 35 GB after three minutes of 40 players scattering
        // (profile 2026-09-23), and the paging it caused slowed the whole
        // server. It was held off for weeks by generation tasks that never
        // ran after a revisit — ChunkMap dropped tasks scheduled from the
        // worldgen lane (see ChunkMap::m_pendingTasksMutex) — and by the
        // one-tick UNKNOWN tickets the requests used, which would have
        // cancelled every request after a tick (GENERATION_REQUEST now).
        {
            PROFILE_ZONE_N("Lib.Tick");
            const auto budget = std::max(m_tickDeadline - std::chrono::milliseconds(2),
                                         std::chrono::steady_clock::now());
            size_t unloaded = 0;
            ForEachLevel([&](ServerLevel& level) {
                auto* gen = level.TerrainGenerator();
                if (!gen) return;
                // Request tickets whose chunk no player's view holds any more
                // (MC: a PLAYER_LOADING ticket exists exactly while the chunk
                // is in a player's range). CancelLoadIfUnwanted releases them
                // as chunks leave a view; this catches the rest — a loader
                // that went away without a leave (a portal far side), a load
                // with no watcher (portal and dragon scans) once it is done.
                // A few hundred checks a tick: a full pass over the held
                // tickets every few seconds.
                {
                    const Game::DimensionId dimension = level.Dimension();
                    ChunkKeeper* keeper = level.Keeper();
                    const auto sessions = m_sessionManager ? m_sessionManager->GetAllSessions()
                                                           : std::vector<std::shared_ptr<PlayerSession>>{};
                    gen->SweepRequestTickets([&](Game::Math::ChunkPos pos) {
                        if (level.pendingChunkLoads.count(pos) != 0 ||
                            level.generationIssued.count(pos) != 0) return true;
                        if (keeper && keeper->Kept(pos)) return true;
                        for (const auto& session : sessions) {
                            if (session && session->KeepsLoaded(dimension, pos)) return true;
                        }
                        return false;
                    }, 256);
                }
                unloaded += gen->TickLibrary(budget);
            });
            m_stats.libraryHoldersUnloaded += unloaded;
        }

        // === 7. AUTOSAVE (MC MinecraftServer.AUTOSAVE_INTERVAL = 6000) ===
        //
        // Five minutes, matching vanilla. Two things depend on it: a crash
        // costs at most five minutes of play rather than the whole session,
        // and shutdown only has recent changes left to flush instead of every
        // chunk generated since launch.
        if (serverTick > 0 && serverTick % 6000 == 0) {
            AutoSave();
        }

        // Sounds played this tick off the server thread (the parallel
        // falling-block and explosion passes) go out now — see
        // ServerSoundBroadcaster.
        if (m_soundBroadcaster) m_soundBroadcaster->Flush();

        // Increment tick counter
        m_stats.ticksProcessed.fetch_add(1, std::memory_order_relaxed);

        // Log server state occasionally
        static uint64_t logCounter = 0;
        if (++logCounter % (m_config.tickRate * 10) == 0) { // Every 10 seconds
            LogServerState();
            
            // Also log session system stats
            if (m_sessionManager) {
                auto sessionStats = m_sessionManager->GetStats();
                Log::Info("Session System: %zu active sessions, %zu chunks/tick, %.1f KB/tick",
                         sessionStats.activeSessions,
                         sessionStats.chunksPerTick,
                         sessionStats.bytesPerTick / 1024.0f);
            }
        }
    }


    // ========================================================================
    // PACKET PROCESSING (Called by NetworkServer callbacks)
    // ========================================================================

    void IntegratedServer::ProcessClientToServerPackets() {
        // This function is now obsolete - packets are processed via NetworkServer callbacks
        // Keeping empty for compatibility during transition
    }

    void IntegratedServer::RequestChunkLoad(Game::DimensionId dimension,
                                            Game::Math::ChunkPos chunkPos, int priority) {
        // GetLevel, NOT GetOrCreateLevel: every caller has already resolved the
        // level it is asking about (a session's, or one it is iterating), and
        // building a whole dimension — terrain generator included — as a side
        // effect of a streaming request is not something a hot path should be
        // able to do by accident.
        ServerLevel* level = GetLevel(dimension);
        if (!level) return;

        Game::World* world = level->World();

        // Already loaded — SendNextChunks() will pick it up
        if (world && world->IsChunkLoaded(chunkPos.x, chunkPos.z)) {
            // ...but its ENTITIES still have to be claimed. RequestLoad is
            // otherwise only reached from ProcessAsyncChunkResults, and a
            // chunk that entered the cache by any other route (the spawn
            // search, a neighbour fetch, a portal scan, a block update) never
            // produces a result — so it never got an entity-store entry, and
            // LevelEntityStore::SaveChunk then refuses to write it because it
            // "was not loaded". Items dropped in such a chunk were silently
            // never saved, which is exactly what happens to the chunk the
            // player is standing in at world load.
            //
            // Idempotent: RequestLoad returns immediately unless the state is
            // Absent, so this costs one map lookup per already-resident chunk.
            if (level->Entities()) level->Entities()->RequestLoad(chunkPos);
            return;
        }

        // Already in flight — a result is coming, and it will push the chunk to
        // every tracking player. Submitting again would generate it twice.
        //
        // The set is PER LEVEL: Overworld (0,0) and Nether (0,0) are the same
        // ChunkPos, so one shared set would make the second request look like a
        // duplicate of the first and that chunk would silently never load.
        if (level->pendingChunkLoads.count(chunkPos) != 0) {
            return;
        }

        // pendingChunkLoads is the "already in flight, don't submit again"
        // guard, and the ONLY thing that clears an entry is a result arriving in
        // ProcessAsyncChunkResults. So a chunk may be marked pending only once a
        // job that is guaranteed to produce a result actually exists —
        // otherwise the entry never clears, the guard suppresses every future
        // request, and that chunk never loads again for the rest of the session.
        // (Symptom: a world loads its spawn chunks and then stops, with new
        // chunks streaming normally once you walk into positions that weren't
        // poisoned.)
        if (m_config.enableAsyncChunkLoading) {
            if (!Threading::SubmitServerChunkLoading(dimension, chunkPos, priority)) {
                // Only reachable when the pool is shutting down — the queue has
                // no capacity limit. Leaving the chunk unmarked is correct:
                // there is no result coming, and nothing is left to retry into.
                return;
            }
            level->pendingChunkLoads.insert(chunkPos);
            Log::Debug("Requested async chunk loading for %s (%d, %d)",
                       std::string(Game::DimensionName(dimension)).c_str(),
                       chunkPos.x, chunkPos.z);
        } else {
            // Sync path: the chunk is in the cache by the time this returns, so
            // push it to its watchers right here — ProcessAsyncChunkResults
            // early-outs entirely when async loading is off, so nothing else
            // ever would. That applies to the chunk's ENTITIES too: the async
            // result path is where RequestLoad normally happens, so without
            // this line sync mode never reads entities/*.mca at all.
            if (world && world->GetChunk(chunkPos.x, chunkPos.z)) {
                if (level->Entities()) level->Entities()->RequestLoad(chunkPos);
                if (m_sessionManager) {
                    m_sessionManager->ForEachSessionWatching(
                        dimension, chunkPos,
                        [&](PlayerSession& session) { session.MarkChunkPendingToSend(dimension, chunkPos); });
                }
            }
        }
    }

    // ========================================================================
    // ITEM ENTITY BROADCAST
    // ========================================================================

    void IntegratedServer::BroadcastItemEntitySpawn(ServerLevel& level, int32_t id) {
        if (!level.Items() || !m_sessionManager) return;

        const auto& all = level.Items()->All();
        auto it = all.find(id);
        if (it == all.end()) return;
        const Game::ItemEntity& e = it->second;

        Network::ItemEntitySpawnS2CPacket packet;
        packet.entityId = e.id;
        packet.position = e.pos;
        packet.velocity = glm::vec3(e.vel);
        packet.bobOffs  = e.bobOffs;
        packet.stack    = e.stack;
        packet.scale    = e.scale;

        const auto data = Network::Serialization::Serialize(packet);
        SendToChunkWatchers(level.Dimension(), e.pos,
                            Network::PacketId::ItemEntitySpawnS2C, data);
    }

    // ========================================================================
    // MOB ENTITIES
    // ========================================================================

    // Factory shared by the natural spawner, /summon, and the entity LOADER.
    //
    // Deliberately not in an anonymous namespace any more: LevelEntityStore
    // needs it to rebuild a saved mob, and a second copy of this switch would
    // silently stop matching the first the next time a mob is added.
    std::unique_ptr<Game::Mob> MakeMobForLoad(Game::EntityTypeId type, Game::EntityLevel* level) {
            switch (type) {
                case Game::EntityTypeId::Zombie:   return std::make_unique<Game::Zombie>(level);
                case Game::EntityTypeId::Skeleton: return std::make_unique<Game::Skeleton>(level);
                case Game::EntityTypeId::Creeper:  return std::make_unique<Game::Creeper>(level);
                case Game::EntityTypeId::Spider:   return std::make_unique<Game::Spider>(level);
                case Game::EntityTypeId::CaveSpider:
                    return std::make_unique<Game::CaveSpider>(level);
                case Game::EntityTypeId::Stray:
                    return std::make_unique<Game::Stray>(level);
                case Game::EntityTypeId::WitherSkeleton:
                    return std::make_unique<Game::WitherSkeleton>(level);
                case Game::EntityTypeId::Bogged:
                    return std::make_unique<Game::Bogged>(level);
                case Game::EntityTypeId::Parched:
                    return std::make_unique<Game::Parched>(level);
                case Game::EntityTypeId::Cow:      return std::make_unique<Game::Cow>(level);
                case Game::EntityTypeId::Pig:      return std::make_unique<Game::Pig>(level);
                case Game::EntityTypeId::Sheep:    return std::make_unique<Game::Sheep>(level);
                case Game::EntityTypeId::Chicken:  return std::make_unique<Game::Chicken>(level);
                case Game::EntityTypeId::Arrow:    return std::make_unique<Game::Arrow>(level);
                case Game::EntityTypeId::Enderman: return std::make_unique<Game::Enderman>(level);
                case Game::EntityTypeId::Husk:     return std::make_unique<Game::Husk>(level);
                case Game::EntityTypeId::Drowned:  return std::make_unique<Game::Drowned>(level);
                case Game::EntityTypeId::ZombieVillager:
                    return std::make_unique<Game::ZombieVillager>(level);
                case Game::EntityTypeId::ZombifiedPiglin:
                    return std::make_unique<Game::ZombifiedPiglin>(level);
                case Game::EntityTypeId::Slime:
                    return Game::Slime::Make(level);
                case Game::EntityTypeId::SulfurCube:
                    return std::make_unique<Game::SulfurCube>(level);
                case Game::EntityTypeId::MagmaCube:
                    return std::make_unique<Game::MagmaCube>(level);
                case Game::EntityTypeId::Cod:
                case Game::EntityTypeId::Salmon:
                    return std::make_unique<Game::SchoolingFish>(type, level);
                // TropicalFish carries MC's 10%-loner spawn roll on top of
                // the schooling base (TropicalFish.java:97,207); the CLIENT
                // mirror stays a plain SchoolingFish — the roll is
                // server-spawn-only state.
                case Game::EntityTypeId::TropicalFish:
                    return std::make_unique<Game::TropicalFish>(level);
                case Game::EntityTypeId::Pufferfish:
                    return std::make_unique<Game::Pufferfish>(level);
                case Game::EntityTypeId::Squid:
                case Game::EntityTypeId::GlowSquid:
                    return std::make_unique<Game::Squid>(type, level);
                case Game::EntityTypeId::Guardian:
                    return std::make_unique<Game::Guardian>(level);
                case Game::EntityTypeId::ElderGuardian:
                    return std::make_unique<Game::ElderGuardian>(level);
                case Game::EntityTypeId::Parrot:
                    return std::make_unique<Game::Parrot>(level);
                case Game::EntityTypeId::IronGolem:
                    return std::make_unique<Game::IronGolem>(level);
                case Game::EntityTypeId::Ravager:
                    return std::make_unique<Game::Ravager>(level);
                case Game::EntityTypeId::Rabbit:
                    return std::make_unique<Game::Rabbit>(level);
                case Game::EntityTypeId::PolarBear:
                    return std::make_unique<Game::PolarBear>(level);
                case Game::EntityTypeId::Fox:
                    return std::make_unique<Game::Fox>(level);
                case Game::EntityTypeId::Turtle:
                    return std::make_unique<Game::Turtle>(level);
                case Game::EntityTypeId::Panda:
                    return std::make_unique<Game::Panda>(level);
                case Game::EntityTypeId::Cat:
                    return std::make_unique<Game::Cat>(level);
                case Game::EntityTypeId::Ocelot:
                    return std::make_unique<Game::Ocelot>(level);
                case Game::EntityTypeId::Dolphin:
                    return std::make_unique<Game::Dolphin>(level);
                case Game::EntityTypeId::HappyGhast:
                    return std::make_unique<Game::HappyGhast>(level);
                case Game::EntityTypeId::Horse:
                    return std::make_unique<Game::Horse>(level);
                case Game::EntityTypeId::Donkey:
                    return std::make_unique<Game::Donkey>(level);
                case Game::EntityTypeId::Mule:
                    return std::make_unique<Game::Mule>(level);
                case Game::EntityTypeId::SkeletonHorse:
                    return std::make_unique<Game::SkeletonHorse>(level);
                case Game::EntityTypeId::ZombieHorse:
                    return std::make_unique<Game::ZombieHorse>(level);
                case Game::EntityTypeId::Blaze:
                    return std::make_unique<Game::Blaze>(level);
                case Game::EntityTypeId::Ghast:
                    return std::make_unique<Game::Ghast>(level);
                case Game::EntityTypeId::Phantom:
                    return std::make_unique<Game::Phantom>(level);
                case Game::EntityTypeId::Vex:
                    return std::make_unique<Game::Vex>(level);
                case Game::EntityTypeId::Evoker:
                    return std::make_unique<Game::Evoker>(level);
                case Game::EntityTypeId::Illusioner:
                    return std::make_unique<Game::Illusioner>(level);
                case Game::EntityTypeId::Vindicator:
                    return std::make_unique<Game::Vindicator>(level);
                case Game::EntityTypeId::Wither:
                    return std::make_unique<Game::Wither>(level);
                case Game::EntityTypeId::EnderDragon:
                    return std::make_unique<Game::EnderDragon>(level);
                case Game::EntityTypeId::Strider:
                    return std::make_unique<Game::Strider>(level);
                case Game::EntityTypeId::SnowGolem:
                    return std::make_unique<Game::SnowGolem>(level);
                case Game::EntityTypeId::Witch:
                    return std::make_unique<Game::Witch>(level);
                case Game::EntityTypeId::Shulker:
                    return std::make_unique<Game::Shulker>(level);
                case Game::EntityTypeId::Llama:
                    return std::make_unique<Game::Llama>(level);
                case Game::EntityTypeId::TraderLlama:
                    return std::make_unique<Game::TraderLlama>(level);
                // ── Projectiles (Arrow precedent: Misc, mob machinery inert) ─
                case Game::EntityTypeId::Snowball:
                    return std::make_unique<Game::Snowball>(level);
                case Game::EntityTypeId::Egg:
                    return std::make_unique<Game::ThrownEgg>(level);
                case Game::EntityTypeId::SplashPotion:
                    return std::make_unique<Game::ThrownSplashPotion>(level);
                case Game::EntityTypeId::SmallFireball:
                    return std::make_unique<Game::SmallFireball>(level);
                case Game::EntityTypeId::Fireball:
                    return std::make_unique<Game::LargeFireball>(level);
                case Game::EntityTypeId::DragonFireball:
                    return std::make_unique<Game::DragonFireball>(level);
                case Game::EntityTypeId::AreaEffectCloud:
                    return std::make_unique<Game::AreaEffectCloud>(level);
                case Game::EntityTypeId::WitherSkull:
                    return std::make_unique<Game::WitherSkull>(level);
                case Game::EntityTypeId::EvokerFangs:
                    return std::make_unique<Game::EvokerFangs>(level);
                case Game::EntityTypeId::ShulkerBullet:
                    return std::make_unique<Game::ShulkerBullet>(level);
                case Game::EntityTypeId::LlamaSpit:
                    return std::make_unique<Game::LlamaSpit>(level);
                case Game::EntityTypeId::Trident:
                    return std::make_unique<Game::ThrownTrident>(level);
                case Game::EntityTypeId::WindCharge:
                    return std::make_unique<Game::WindCharge>(level);
                case Game::EntityTypeId::BreezeWindCharge:
                    return std::make_unique<Game::BreezeWindCharge>(level);
                // Without this case a saved eye of ender reloads as a generic
                // mob: the type id round-trips, so nothing looks wrong, but
                // the concrete class is gone and its NBT applies to nothing.
                case Game::EntityTypeId::EyeOfEnder:
                    return std::make_unique<Game::EyeOfEnder>(level);
                // MC LightningBolt (/summon lightning_bolt resolves here). Never
                // saved (noSave), so this is the summon/spawn path only.
                case Game::EntityTypeId::LightningBolt:
                    return std::make_unique<Game::LightningBolt>(level);
                // ── Block-shaped entities — MUST mirror the client factory ──
                // Neither is a Mob in MC; both ride this pipeline for the same
                // reason the projectiles do (see FallingBlockEntity.hpp).
                case Game::EntityTypeId::FallingBlock:
                    return std::make_unique<Game::FallingBlockEntity>(level);
                case Game::EntityTypeId::Tnt:
                    return std::make_unique<Game::PrimedTnt>(level);
                case Game::EntityTypeId::EndCrystal:
                    return std::make_unique<Game::EndCrystal>(level);
                case Game::EntityTypeId::EnderPearl:
                    return std::make_unique<Game::ThrownEnderpearl>(level);
                // MC's ArmorStand is a LivingEntity, not a Mob — see
                // ArmorStand.hpp for why it rides this pipeline.
                case Game::EntityTypeId::ArmorStand:
                    return std::make_unique<Game::ArmorStand>(level);
                default: break;
            }
            // Everything else is built from its generated def. Promoting one to
            // a hand-written class is purely additive: add a case above and the
            // generic path stops being used for it.
            return Game::MakeGenericMob(type, level);
        }
    

    void IntegratedServer::TickMobs(ServerLevel& level, int64_t serverTick) {
        MobManager*          mobs     = level.Mobs();
        ServerLevelBridge*   mobLevel = level.MobLevel();
        ServerEntityTracker* tracker  = level.MobTracker();
        if (!mobs || !mobLevel || !tracker || !m_sessionManager) return;

        PROFILE_ZONE_N("MobSystemTick");

        // 0. MC DistanceManager.runAllUpdates. The tick's pass ran after the
        //    sessions; this one picks up tickets added since (a portal's far
        //    side, an ender pearl), so every per-entity gate below — a
        //    lock-free hash lookup — reads them this tick. Nothing queued:
        //    one check and out.
        if (level.Tickets()) level.Tickets()->RunAllUpdates();

        //    Reset this tick's detonation budget. See
        //    EntityLevel::TryBeginExplosion for why this exists and what it
        //    deliberately diverges from.
        mobLevel->BeginExplosionBudget();

        // 1. Refresh the player views FIRST. Everything downstream — targeting,
        //    despawn distance, the tracker's watch sets — reads them, and a
        //    stale view is a dangling ServerPlayer pointer.
        mobLevel->SyncPlayerViews();

#if ENABLE_IMMERSIVE_PORTALS
        // 1b. The gun surfaces this level's mobs may walk through — collected
        //     here, before any mob moves, and read-only while they tick
        //     (see MobPortalCollision).
        mobLevel->PortalCollisionState().Refresh(ImmersivePortals(), level.Dimension());
#endif

        // 2. Tick, scoped to the block-ticking chunk set (mobs outside it are
        //    still despawn-checked; see MobManager::Tick).
        std::vector<int32_t> removed;

        // No set is built. MobManager reads entity-ticking range live from each
        // mob's own chunk, exactly as MC's ServerLevel.tick does — see the note
        // on MobManager::Tick.
        // 2a. The Hush's stillness: its timer steps and raises the bridge's
        //     flag BEFORE the mobs tick, so a stillness that begins this tick
        //     already holds them this tick (HushStillness.hpp).
        if (auto* stillness = level.Stillness()) stillness->Tick();
        mobs->Tick(level.Tickets(), removed);
        // The compact falling blocks, right after the mobs (where the Mob-
        // shaped falling batch runs too) and before the spawner.
        if (level.FallingBlocks()) level.FallingBlocks()->Tick(level.Tickets());

        // 3. Spawn. After ticking so this tick's despawns have already freed
        //    room under the caps.
        RunNaturalSpawner(level, serverTick);

        // 3b. The End's dragon fight (MC ServerLevel.tick's dragonFight.tick).
        //     After the mobs so it sees this tick's deaths, before the emit so
        //     a dragon it spawns is tracked this same tick.
        if (auto* fight = level.DragonFightController()) fight->Tick();
        // 3c. The Silent Warden's boss bar (The Hush) — same slot, same
        //     reasons: after the mobs so a death this tick pulls the bar.
        if (auto* bars = level.WardenBossBars()) bars->Tick();
        // 3d. Lighthouses that guide: at most one lamp's Aurelith lookup.
        LighthouseGuide::Tick(level);
        // 3e. Aurelith's cities (AurelithCities.hpp): the awakening's
        //     timeline and light wave, the fight, the clients' records.
        //     After the mobs, so the Unsung's death this tick is seen.
        if (auto* cities = level.Aurelith()) cities->Tick();

        // 4. Emit — the client-facing half, shared with the frozen world.
        SyncMobsToClients(level, removed);
    }

    void IntegratedServer::SyncMobsToClients(ServerLevel& level,
                                             const std::vector<int32_t>& removed) {
        // MC ChunkMap's entity tracking runs every server tick whether or not
        // the world simulates (ServerLevel.tick: the tick-rate manager
        // freezes entity TICKING, ChunkMap.tick(BooleanSupplier) still tracks)
        // — which is what lets a frozen world show the arrows a player shoots
        // and the mobs they spawn the moment they exist. Split out of
        // TickMobs so the frozen path can run it alone.
        MobManager*          mobs     = level.Mobs();
        ServerLevelBridge*   mobLevel = level.MobLevel();
        ServerEntityTracker* tracker  = level.MobTracker();
        if (!mobs || !mobLevel || !tracker || !m_sessionManager) return;

        PROFILE_ZONE_N("MobSync");

        // The tracker owns who-knows-what, so removals go through it rather
        // than being broadcast blindly.
        std::vector<EntityPacketOut> outgoing;
        // Entity events BEFORE removals: an event raised in the same tick as
        // the discard (creeper explosion, projectile impact burst) must be
        // emitted while the entity is still tracked here and still exists on
        // the client — RemoveEntity erases the watcher set, and the client
        // ignores events for ids it no longer knows. Events for mobs not
        // tracked yet (added this tick) wait for Tick's closing flush.
        tracker->FlushEntityEvents(*mobLevel, outgoing, /*keepUntracked=*/true);
        tracker->RemoveEntities(removed, outgoing);

        // Only the players standing in THIS level. A player in the Overworld
        // must not appear in the Nether tracker's distance tests, or the Nether
        // would spawn-and-track its mobs for someone who will never see them.
        std::vector<ServerEntityTracker::TrackedPlayer> players;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (!session || !session->GetPlayer()) continue;
            // One entry per LOADER this session has in this level: where the
            // player stands, and every portal far side they can look into.
            // The tracker ORs the entries of one connection, so an entity is
            // visible if ANY of them reaches it — the mod's "watched chunk
            // within tracking range of its loader" rule.
            for (const ChunkLoader& loader : session->Loaders()) {
                if (loader.dimension != level.Dimension()) continue;
                if (loader.IsSimulation()) continue;     // loaded for ticking only: the client sees none of it
                ServerEntityTracker::TrackedPlayer tp;
                tp.connectionId = session->GetConnectionId();
                if (loader.source == ChunkLoader::Source::Player) {
                    tp.position = session->GetPlayer()->getPosition();
                } else {
                    tp.position = glm::dvec3(loader.Center().x * 16.0 + 8.0,
                                             session->GetPlayer()->getPosition().y,
                                             loader.Center().z * 16.0 + 8.0);
                }
                // MC ChunkMap.getPlayerViewDistance — clamps the tracking range.
                tp.viewDistance = loader.Radius();
                // MC ChunkMap.isChunkTracked — entity updates never precede the
                // chunk they stand in.
                tp.sentChunks   = &session->GetSentChunks(level.Dimension());
                players.push_back(tp);
            }
        }

        tracker->Tick(*mobs, *mobLevel, players, level.Tickets(), outgoing);
        if (level.FallingBlocks()) level.FallingBlocks()->Track(players, level.Tickets(), outgoing);

        // 5. Send. One lookup per recipient rather than per packet — a busy
        //    tick emits hundreds of packets across a handful of connections.
        for (const auto& packet : outgoing) {
            auto session = m_sessionManager->GetSession(packet.connectionId);
            if (!session || !session->GetConnection()) continue;
            session->GetConnection()->SendPacketIn(level.Dimension(),
                                                   static_cast<uint8_t>(packet.packetId),
                                                   packet.payload);
        }

        // 6. Knockback the mob system applied to players. The player is
        //    client-authoritative for movement, so a push has to be SENT as a
        //    velocity packet or the next move packet simply overwrites it.
        for (Server::PlayerEntityView* view : mobLevel->PlayerViews()) {
            glm::dvec3 push;
            if (!view->ConsumePendingKnockback(push)) continue;

            Network::SetEntityMotionS2CPacket motion;
            motion.entityId = view->GetId();
            motion.velocity = glm::vec3(push);

            auto session = m_sessionManager->GetSession(static_cast<uint32_t>(view->GetId()));
            if (session && session->GetConnection()) {
                session->GetConnection()->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::SetEntityMotionS2C),
                    Network::Serialization::Serialize(motion));
            }
        }
    }

    void IntegratedServer::TickPortals(ServerLevel& level) {
        Game::World*       world    = level.World();
        ServerLevelBridge* mobLevel = level.MobLevel();
        if (!world || !mobLevel) return;

        PROFILE_ZONE_N("PortalTick");

        // Aether EntityMixin (the entity tick hook): a player at or below the
        // Aether's floor who is not riding anything falls into the
        // Overworld's sky (PortalTravel::FallOutOfAether, entityFell).
        // Collected first: the move hands the player's view to another
        // level, and the list must not change under the walk.
        if (level.Dimension() == Game::DimensionId::Aether) {
            const double floorY = static_cast<double>(Game::DimensionMinY(Game::DimensionId::Aether));
            std::vector<Server::PlayerEntityView*> fallen;
            for (Server::PlayerEntityView* view : mobLevel->PlayerViews()) {
                if (!view || view->IsPassenger()) continue;
                if (view->position.y <= floorY) fallen.push_back(view);
            }
            for (Server::PlayerEntityView* view : fallen) {
                PortalTravel::FallOutOfAether(*this, level, *view);
            }
        }

        // MC Entity.canUsePortal is asked twice per tick for a reason: once by
        // entityInside to decide whether to START accumulating, and once by
        // handlePortal to decide whether the accumulated time may FIRE. An
        // entity that dies mid-crossing must not arrive.
        auto tickOne = [this, world, &level](Game::Entity& entity) {
            // Contact first — this is what re-arms `insidePortalThisTick` for
            // any portal cell the entity currently overlaps. Skipping it is
            // indistinguishable from stepping out of the portal, and the
            // 4-ticks-per-tick decay would eat the progress.
            //
            // Against THIS level's blocks: the entity is standing in this
            // world, and the same coordinates in another one hold something
            // else entirely.
            entity.CheckInsideBlocks(*world);

#if ENABLE_IMMERSIVE_PORTALS
            // A frame-portal block of an immersive family is scenery; the
            // see-through surface handles crossing (End and Twilight portals
            // are always blocks). Per family: the nether follows /gamerule
            // immersive_portals, the Hush and Aether are always blocks.
            if (entity.portal.IsInPortal() &&
                Game::Portals::IsInertPortalBlock(entity.portal.CurrentPortal())) {
                entity.portal.Reset();
                return;
            }
#endif

            if (!entity.portal.IsInPortal() && !entity.portal.IsOnCooldown()) {
                return;   // nothing to advance; the overwhelmingly common case
            }

            const Game::BlockID active = entity.portal.CurrentPortal();
            const int transitionTime = Game::Portals::GetTransitionTime(
                active, entity.IsPlayer(),
                entity.IsCreative() || entity.IsSpectator());

            // MC ServerPlayer.processPortalCooldown: a player whose
            // cross-dimension teleport the client has not acked yet keeps
            // the whole arrival cooldown for when it can actually move.
            bool processCooldown = true;
            if (entity.IsPlayer()) {
                if (const auto* view = dynamic_cast<const Server::PlayerEntityView*>(&entity)) {
                    if (const Server::ServerPlayer* p = view->GetPlayer()) {
                        processCooldown = !p->isChangingDimension();
                    }
                }
            }

            const auto trigger = entity.portal.HandleTick(
                transitionTime, entity.CanUsePortal(false),
                entity.GetDimensionChangingDelay(), processCooldown);
            if (!trigger) return;

            HandlePortalTraversal(level, entity, trigger->portal, trigger->entryPos);
        };

        // Players first, so a player and a mob crossing on the same tick
        // resolve in a deterministic order.
        for (Server::PlayerEntityView* view : mobLevel->PlayerViews()) {
            if (view) tickOne(*view);
        }
        if (MobManager* mobs = level.Mobs()) {
            // ENTITY-TICKING GATE. MC reaches checkInsideBlocks through
            // Entity.baseTick, which only runs for entities in
            // `entityTickList` — i.e. entity-ticking chunks
            // (ServerLevel.java:371, :1810). This loop used to call
            // CheckInsideBlocks on EVERY mob in the level before any early-out,
            // which is both a divergence and a per-entity block scan: measured
            // at 5.08 ms/tick with a 114 ms peak on a 100,000-entity world.
            //
            // Lock-free, same as MobManager's gate: RunAllUpdates() ran at the
            // top of TickMobSystems.
            //
            // TWO PASSES. Everything a mob's turn does before it can write
            // anything — the parked and stagger skips, the ticking gate, and
            // CheckInsideBlocks' own all-air early-out over the swept region —
            // is read-only, so it runs across the pool and leaves one byte
            // per mob: does this one need its full turn? The serial pass then
            // visits only those. With a hundred thousand airborne entities
            // the serial walk alone was 5 ms a tick, nearly all of it cache
            // misses to conclude "air".
            const ChunkTicketManager* tickets = level.Tickets();
            const auto& list = mobs->List();
            const size_t n = list.size();
            // The server thread's own buffer, bound to a reference the lambda
            // captures: a bare thread_local inside the lambda would resolve to
            // each WORKER's (empty) instance and write off the end of it.
            thread_local std::vector<uint8_t> t_needsTurnStorage;
            std::vector<uint8_t>& needsTurn = t_needsTurnStorage;
            needsTurn.resize(n);
            const auto prefilter = [&](size_t i) {
                const Game::Mob* mob = list[i];
                needsTurn[i] = 0;
                if (!mob) return;
                const bool inPortalOrCooling =
                    mob->portal.IsInPortal() || mob->portal.IsOnCooldown();
                // Parked FIRST, before the ticking-gate hash: a parked mob
                // (see PrimedTnt::Tick) has not moved and its columns have
                // not been written since it parked — the swept inside-block
                // scan cannot find anything it did not find when it parked.
                // A portal lit under it writes blocks, unparks it, and the
                // scan runs again. At a million parked TNT the ordering of
                // these two tests is tens of milliseconds a tick.
                if (mob->physicsParked && !inPortalOrCooling) return;
                // Moving TNT and falling blocks scan for portals on every
                // fourth tick only (staggered by id): entering a portal
                // registers up to 150 ms late, and a cascade with hundreds of
                // thousands of airborne TNT — or a collapsing sand pyramid
                // with as many falling blocks — stops paying a full swept
                // block scan per entity per tick for a structure the bench
                // world does not even contain.
                if ((mob->GetType() == Game::EntityTypeId::Tnt ||
                     mob->GetType() == Game::EntityTypeId::FallingBlock) &&
                    !inPortalOrCooling &&
                    ((static_cast<uint32_t>(mob->tickCount) +
                      static_cast<uint32_t>(mob->GetId())) & 3u) != 0u) {
                    return;
                }
                if (tickets) {
                    const Game::Math::ChunkPos mobChunk(
                        static_cast<int>(std::floor(mob->position.x)) >> 4,
                        static_cast<int>(std::floor(mob->position.z)) >> 4);
                    if (!tickets->IsEntityTickingAfterUpdates(mobChunk)) return;
                }
                // Mid-crossing or cooling down: the state machine has to
                // advance whatever the blocks say.
                if (inPortalOrCooling) { needsTurn[i] = 1; return; }
                // Otherwise the turn is CheckInsideBlocks alone, and its first
                // act is this same all-air test — answered here, read-only.
                glm::ivec3 lo, hi;
                mob->SweptInsideRegion(lo, hi);
                needsTurn[i] = world->IsRegionAllAir(lo, hi) ? 0 : 1;
            };
            if (n >= 2048 && Core::ParallelWidth() > 1) {
                Core::ParallelFor(n, 512, prefilter);
            } else {
                for (size_t i = 0; i < n; ++i) prefilter(i);
            }
            for (size_t i = 0; i < n; ++i) {
                if (needsTurn[i]) tickOne(*list[i]);
            }
        }
    }

    void IntegratedServer::HandlePortalTraversal(ServerLevel& from, Game::Entity& entity,
                                                 Game::BlockID portal,
                                                 const glm::ivec3& entryPos) {
        // The whole destination half lives in PortalTravel because it is the
        // only code that legitimately reads TWO levels at once, and keeping it
        // in one file is what makes "which world am I looking at" answerable.
        //
        // The cooldown is already armed by the time we get here
        // (PortalState::HandleTick does it before returning the trigger), so a
        // failed resolution costs one attempt rather than one per tick.
        PortalTravel::Traverse(*this, from, entity, portal, entryPos);
    }

    void IntegratedServer::RunNaturalSpawner(ServerLevel& level, int64_t serverTick) {
        PROFILE_ZONE_N("NaturalSpawner");
        MobManager*        mobs     = level.Mobs();
        ServerLevelBridge* mobLevel = level.MobLevel();
        Game::World*       world    = level.World();
        if (!mobs || !mobLevel || !world || !level.Tickets()) return;

        // MC ServerLevel.tickChunk gates the spawner on doMobSpawning. Only the
        // spawner — despawning and AI keep running, so the world drains rather
        // than freezing when it is turned off.
        if (!world->GetDoMobSpawning()) return;

        std::vector<glm::dvec3> playerPositions;
        for (Server::PlayerEntityView* view : mobLevel->PlayerViews()) {
            if (view->IsSpectator()) continue;
            playerPositions.push_back(view->position);
        }
        if (playerPositions.empty()) return;   // nobody to spawn for

        // ── Spawnable chunk accounting (MC DistanceManager + ChunkMap) ─────
        //
        // MC's naturalSpawnChunkCounter is a radius-8 player tracker:
        // spawnableChunkCount is the size of the UNION of 17x17 chunk squares
        // around every player — REGARDLESS of ticking or loaded state. That is
        // the denominator's whole meaning: one player far from another always
        // contributes 289, so the global cap scales with players, not with how
        // many chunks happen to be resident.
        //
        // Only the union's SIZE is needed: every chunk that gets spawn
        // attempts passes the 128-block test below, which already puts it in
        // that player's square (a chunk centre within 128 blocks of a player
        // is at most 8 chunks from the player's chunk on each axis). Counted
        // by sort + unique — a hash set of 289 keys per player was the
        // spawner's largest fixed per-tick cost with a few dozen players.
        std::vector<uint64_t> spawnSquares;
        spawnSquares.reserve(playerPositions.size() * Game::kMagicNumber);
        for (const glm::dvec3& p : playerPositions) {
            const int pcx = static_cast<int>(std::floor(p.x)) >> 4;
            const int pcz = static_cast<int>(std::floor(p.z)) >> 4;
            for (int dx = -Game::kSpawnDistanceChunk; dx <= Game::kSpawnDistanceChunk; ++dx) {
                for (int dz = -Game::kSpawnDistanceChunk; dz <= Game::kSpawnDistanceChunk; ++dz) {
                    spawnSquares.push_back(
                        (static_cast<uint64_t>(static_cast<uint32_t>(pcx + dx)) << 32) |
                         static_cast<uint32_t>(pcz + dz));
                }
            }
        }
        std::sort(spawnSquares.begin(), spawnSquares.end());
        const size_t spawnableChunkCount = static_cast<size_t>(
            std::unique(spawnSquares.begin(), spawnSquares.end()) - spawnSquares.begin());

        Game::SpawnContext ctx;
        ctx.level = mobLevel;
        ctx.spawnableChunkCount = static_cast<int>(spawnableChunkCount);
        mobs->SetSpawnableChunkCount(ctx.spawnableChunkCount);
        ctx.playerPositions = &playerPositions;

        // The chunks that actually RECEIVE spawn attempts are narrower — MC
        // ChunkMap.collectSpawningChunks: in a spawn square, holding a ticking
        // (here: block-ticking AND loaded) chunk, and with the chunk CENTER
        // within 128 blocks of a non-spectator player, XZ only. The random-
        // ticking list is the block-ticking list within vanilla's reach of a
        // player — every chunk the 128-block test can pass is in it.
        auto tickingChunks = level.Tickets()->GetRandomTickingChunks();
        tickingChunks.erase(
            std::remove_if(tickingChunks.begin(), tickingChunks.end(),
                           [world](const Game::Math::ChunkPos& cp) {
                               return !world->IsChunkLoaded(cp.x, cp.z);
                           }),
            tickingChunks.end());

        const auto playerCloseToChunk = [&playerPositions](int cx, int cz) {
            const double centerX = cx * 16.0 + 8.0;
            const double centerZ = cz * 16.0 + 8.0;
            for (const glm::dvec3& p : playerPositions) {
                const double dx = p.x - centerX;
                const double dz = p.z - centerZ;
                if (dx * dx + dz * dz < 128.0 * 128.0) return true;   // 16384 = 128²
            }
            return false;
        };

        std::vector<Game::Math::ChunkPos> chunks;
        chunks.reserve(tickingChunks.size());
        for (const auto& cp : tickingChunks) {
            if (playerCloseToChunk(cp.x, cp.z)) chunks.push_back(cp);
        }

        ctx.biomeAt = [world](int x, int y, int z) -> std::string_view {
            // BiomeInfo::name is the bare vanilla slug ("plains"), which is
            // exactly the key GeneratedMobSpawns is built on — so no
            // translation table is needed between the two.
            return Game::BiomeRegistry::Get(world->GetBiome(x, y, z)).name;
        };
        ctx.biomeIdAt = [world](int x, int y, int z) -> Game::BiomeId {
            return world->GetBiome(x, y, z);
        };

        // MC NaturalSpawner.mobsAt's structure half: the fortress rule and
        // every structure's spawn_overrides, answered from the boxes the
        // chunk recorded at generation (Chunk::structureSpawnAreas). The
        // block-below read only happens inside a chunk that has any.
        ctx.structureSpawnsAt = [world](Game::MobCategory category, int x, int y, int z)
                -> const Game::BiomeSpawnList* {
            const auto chunk = world->GetLoadedChunk(x >> 4, z >> 4);
            if (!chunk || !Game::StructureSpawns::ChunkHasOverrides(*chunk)) return nullptr;
            return Game::StructureSpawns::MobsAt(
                *chunk, category, x, y, z, world->GetBlock(x, y - 1, z),
                Game::SpawnListForBiome(world->GetBiome(x, y, z)));
        };

        // ── Census (MC NaturalSpawner.createState) ─────────────────────────
        //
        // Rebuilt from scratch every spawn tick over every live mob. Mobs that
        // cannot despawn — persistence flag or state-based immunity — do NOT
        // count toward any cap, which is why name-tagged mobs in vanilla never
        // choke a farm. MISC never counts either.
        Server::LocalMobCapCalculator localCap(&playerPositions);
        Game::PotentialCalculator spawnPotential;
        int categoryCounts[Game::kMobCategoryCount] = {};
        for (const auto& [id, mob] : mobs->All()) {
            PROFILE_ZONE_DETAIL("Spawn.CensusMob");
            if (mob->IsRemoved()) continue;
            if (mob->IsPersistenceRequired() || mob->RequiresCustomPersistence()) continue;
            const Game::MobCategory category = mob->TypeInfo().category;
            if (category == Game::MobCategory::Misc) continue;

            const glm::ivec3 bp = mob->BlockPosition();
            const auto* biomeList = Game::SpawnListForBiome(world->GetBiome(bp.x, bp.y, bp.z));
            if (const auto* cost = Game::FindMobSpawnCost(biomeList, mob->GetType())) {
                spawnPotential.AddCharge(bp, cost->charge);
            }
            localCap.AddMob(bp.x >> 4, bp.z >> 4, category);
            ++categoryCounts[static_cast<size_t>(category)];
        }
        ctx.categoryCounts = categoryCounts;
        ctx.spawnPotential = &spawnPotential;
        // MC NaturalSpawner.getFilteredSpawningCategories: with
        // spawn_monsters off the MONSTER category is left out entirely
        // (ServerLevel.canSpawnMonsters = spawn_mobs && spawn_monsters).
        const bool spawnMonsters = Game::Rules::GetBool(Game::Rules::Id::SpawnMonsters);
        ctx.canSpawnLocal = [&localCap, spawnMonsters](Game::MobCategory category, int cx, int cz) {
            // The Aether's monster categories are MONSTER for this rule.
            if (Game::IsMonsterCategory(category) && !spawnMonsters) return false;
            return localCap.CanSpawn(category, cx, cz);
        };
        ctx.afterSpawn = [&localCap](Game::MobCategory category, const glm::ivec3& pos) {
            localCap.AddMob(pos.x >> 4, pos.z >> 4, category);
        };

        // MC Level.noCollision(type.getSpawnAABB(...)) — spawn dimensions are
        // SCALED for slime/magma cube, which spawn at up to size 4.
        ctx.spawnBoxFree = [world](Game::EntityTypeId type,
                                   double x, double y, double z) -> bool {
            const Game::EntityTypeInfo& info = Game::GetEntityTypeInfo(type);
            const float scale = Game::GetSpawnDimensionsScale(type);
            const float half = info.width * scale * 0.5f;
            Game::AABB box;
            box.min = glm::vec3(x - half, y, z - half);
            box.max = glm::vec3(x + half, y + info.height * scale, z + half);

            Game::PhysicsContext phys;
            phys.blockAccess = world;
            return !Game::CollidesAt(box, phys);
        };

        // MC ServerLevel.canSpawnEntitiesInChunk. The pack walk drifts out of
        // the origin chunk routinely; MC only lets it land in another chunk
        // that is itself entity-ticking, which is exactly this set. Indexed
        // because it is queried once per placement attempt, not once per chunk.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>
            tickingSet(tickingChunks.begin(), tickingChunks.end());
        ctx.chunkSpawnable = [&tickingSet](int cx, int cz) {
            return tickingSet.find(Game::Math::ChunkPos{ cx, cz }) != tickingSet.end();
        };

        // MC isRightDistanceToPlayerAndSpawnPoint's respawn-point half — the
        // LEVEL's shared spawn, not any particular player's bed.
        if (m_sessionManager) {
            ctx.worldSpawn = glm::dvec3(m_sessionManager->GetWorldSpawn());
            ctx.hasWorldSpawn = true;
        }

        Game::EntityLevel* entityLevel = mobLevel;
        ctx.createMob = [entityLevel](Game::EntityTypeId type) {
            return MakeMobForLoad(type, entityLevel);
        };
        ctx.addFreshEntity = [mobs](std::unique_ptr<Game::Mob> mob) {
            mobs->Add(std::move(mob));
        };

        ctx.minY = Game::Math::WorldCoordinates::MIN_WORLD_Y;
        // MC ServerLevel.getSeaLevel, which comes from the DIMENSION's noise
        // settings — 63 / 32 / 0. It splits the spawner's water and underground
        // categories, so the Nether's 32 is what stops the whole lava sea
        // reading as "above sea level".
        ctx.seaLevel = level.SeaLevel();
        ctx.worldSeed = world->GetGenerationSeed();
        ctx.surfaceHeight = [world](int x, int z) -> int {
            auto chunk = world->GetLoadedChunk(x >> 4, z >> 4);
            if (!chunk) return std::numeric_limits<int>::min();  // fail-closed: bat rule rejects
            if (!chunk->AreHeightmapsPrimed()) chunk->PrimeHeightmaps();
            return chunk->GetSurfaceHeight(x & 15, z & 15,
                                           Game::HeightmapType::WorldSurface);
        };

        // ── The per-tick category filter (MC getFilteredSpawningCategories) ─
        //
        // spawnFriendlies is hardcoded true in MC's tickChunks; spawnEnemies is
        // the peaceful gate; the persistent categories (CREATURE) only pass
        // every 400 ticks.
        const bool spawnEnemies =
            mobLevel->GetDifficulty() != Game::Difficulty::Peaceful;
        const bool spawnPersistent =
            (serverTick % Game::kCreatureSpawnInterval) == 0;
        const std::vector<Game::MobCategory> categories =
            Game::GetFilteredSpawningCategories(ctx, /*spawnFriendlies=*/true,
                                                spawnEnemies, spawnPersistent);
        if (categories.empty() || chunks.empty()) return;

        // MC shuffles the spawnable chunk list and walks it. Walking it in a
        // fixed order would bias spawning toward whichever chunks happen to
        // hash first, which shows up as mobs clustering on one side of a player.
        Game::JavaRandom& rng = mobLevel->Random();
        for (size_t i = chunks.size(); i > 1; --i) {
            std::swap(chunks[i - 1], chunks[rng.NextInt(static_cast<int>(i))]);
        }

        for (const auto& pos : chunks) {
            // MC hands spawnForChunk the resolved LevelChunk. We filtered the
            // list to loaded chunks above, so this is a cache hit.
            auto chunk = world->GetLoadedChunk(pos.x, pos.z);
            if (!chunk) continue;
            // MC's heightmaps are always live; ours can be unprimed on a chunk
            // that skipped both the generator copy and the NBT restore, and
            // getRandomPosWithin would then sample against MIN_Y.
            if (!chunk->AreHeightmapsPrimed()) chunk->PrimeHeightmaps();
            Game::SpawnForChunk(ctx, *chunk, pos.x, pos.z, categories, rng);
        }
    }


    // ── /shape build job ────────────────────────────────────────────────────
    //
    // One job at a time, streamed at a fixed cell budget per tick so a
    // hundred-cubed cube (a million writes) lands over a handful of ticks
    // instead of one multi-second stall. State is file-local: there is one
    // integrated server, and a fresh Initialize resets it via the ctor path.
    namespace {
        struct ShapeJobState {
            ShapeJobRequest job{};
            bool     active = false;
            int64_t  cursor = 0;      // linear index over the bounding box
            int64_t  placed = 0;
            glm::ivec3 lo{0}, size{0};
            // Builds submitted while one is streaming wait their turn — a
            // burst of /shape commands in one chat line should all land.
            std::deque<ShapeJobRequest> queue;
        };
        ShapeJobState s_shape;

        // Start streaming `job` (assumes no job is active).
        void BeginShapeJob(const ShapeJobRequest& job) {
            s_shape.job    = job;
            s_shape.cursor = 0;
            s_shape.placed = 0;
            glm::ivec3 sz;
            switch (job.form) {
                case ShapeForm::Box:      sz = glm::ivec3(job.a, job.b, job.c); break;
                case ShapeForm::Wall:
                    sz = job.facing.x != 0 ? glm::ivec3(1, job.b, job.a)
                                           : glm::ivec3(job.a, job.b, 1);
                    break;
                case ShapeForm::Sphere:   sz = glm::ivec3(2 * job.a + 1); break;
                case ShapeForm::Dome:     sz = glm::ivec3(2 * job.a + 1, job.a + 1, 2 * job.a + 1); break;
                case ShapeForm::Cylinder: sz = glm::ivec3(2 * job.a + 1, job.b, 2 * job.a + 1); break;
                case ShapeForm::Pyramid:  sz = glm::ivec3(job.a, job.a / 2 + 1, job.a); break;
            }
            s_shape.size = sz;
            s_shape.lo   = glm::ivec3(job.base.x - sz.x / 2, job.base.y, job.base.z - sz.z / 2);
            s_shape.active = true;
        }

        // Is this bounding-box cell part of the requested form?
        bool ShapeContains(const ShapeJobRequest& j, const glm::ivec3& sz,
                           int x, int y, int z) {
            // x/z relative to the box, y from the bottom.
            const int cx = sz.x / 2, cz = sz.z / 2;
            switch (j.form) {
                case ShapeForm::Box: {
                    if (j.frame) {
                        int edges = 0;
                        if (x == 0 || x == sz.x - 1) ++edges;
                        if (y == 0 || y == sz.y - 1) ++edges;
                        if (z == 0 || z == sz.z - 1) ++edges;
                        return edges >= 2;
                    }
                    if (j.hollow) {
                        return x == 0 || x == sz.x - 1 || y == 0 || y == sz.y - 1 ||
                               z == 0 || z == sz.z - 1;
                    }
                    return true;
                }
                case ShapeForm::Wall: {
                    if (j.frame || j.hollow) {
                        return x == 0 || x == sz.x - 1 || y == 0 || y == sz.y - 1;
                    }
                    return true;
                }
                case ShapeForm::Sphere:
                case ShapeForm::Dome: {
                    const double r  = j.a + 0.5;
                    const double dx = x - cx, dz = z - cz;
                    const double dy = y - (j.form == ShapeForm::Dome ? 0 : j.a);
                    const double d  = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (j.form == ShapeForm::Dome && y < 0) return false;
                    if (d > r) return false;
                    if (j.hollow || j.frame) return d > r - 1.25;
                    return true;
                }
                case ShapeForm::Cylinder: {
                    const double r  = j.a + 0.5;
                    const double dx = x - cx, dz = z - cz;
                    const double d  = std::sqrt(dx * dx + dz * dz);
                    if (d > r) return false;
                    // hollow: an open tube — walls only, no caps, so you can
                    // stand inside it (or fill it with primed TNT).
                    if (j.hollow || j.frame) return d > r - 1.25;
                    return true;
                }
                case ShapeForm::Pyramid: {
                    // Stepped: each level shrinks by one block a side.
                    const int half = j.a / 2 - y;
                    if (half < 0) return false;
                    const int ax = std::abs(x - cx), az = std::abs(z - cz);
                    if (ax > half || az > half) return false;
                    if (j.hollow || j.frame) {
                        return ax == half || az == half || y == 0;
                    }
                    return true;
                }
            }
            return false;
        }
    } // namespace

    bool IntegratedServer::SubmitShapeJob(const ShapeJobRequest& job) {
        if (s_shape.active) {
            // Queue rather than refuse — several /shape commands issued in a
            // burst (or one chat line) all land, in order.
            if (s_shape.queue.size() >= 8) return false;
            s_shape.queue.push_back(job);
            return true;
        }
        BeginShapeJob(job);
        return true;
    }

    void IntegratedServer::TickShapeJob() {
        if (!s_shape.active) return;
        Game::World* world = Overworld().World();
        if (!world) { s_shape.active = false; return; }

        const ShapeJobRequest& j = s_shape.job;
        const glm::ivec3 sz = s_shape.size;
        const int64_t total = int64_t(sz.x) * sz.y * sz.z;

        // Budgets: cells examined bounds the membership math, writes bound
        // the real work. Roughly one to two milliseconds a tick either way.
        constexpr int64_t kCellsPerTick  = 600000;
        constexpr int64_t kWritesPerTick = 120000;

        int64_t cells = 0, writes = 0;
        while (s_shape.cursor < total && cells < kCellsPerTick && writes < kWritesPerTick) {
            const int64_t idx = s_shape.cursor++;
            ++cells;
            // y-major: the shape rises out of the ground tick by tick.
            const int y = static_cast<int>(idx / (int64_t(sz.x) * sz.z));
            const int rem = static_cast<int>(idx % (int64_t(sz.x) * sz.z));
            const int z = rem / sz.x;
            const int x = rem % sz.x;

            if (j.checker && (((x + y + z) & 1) != 0)) continue;
            if (j.spaced > 1 && (x % j.spaced || y % j.spaced || z % j.spaced)) continue;
            if (!ShapeContains(j, sz, x, y, z)) continue;

            if (world->SetBlock(s_shape.lo.x + x, s_shape.lo.y + y, s_shape.lo.z + z,
                                j.block, Game::World::UpdateFlags::All)) {
                ++writes;
                ++s_shape.placed;
            }
        }

        if (s_shape.cursor >= total) {
            s_shape.active = false;
            Log::Info("[Shape] done: %lld blocks placed",
                      static_cast<long long>(s_shape.placed));
            BroadcastSystemMessage("[Shape] done: " +
                std::to_string(s_shape.placed) + " blocks placed", 0xFFFFFFFF);
            if (!s_shape.queue.empty()) {
                const ShapeJobRequest next = s_shape.queue.front();
                s_shape.queue.pop_front();
                BeginShapeJob(next);
            }
        }
    }

    int IntegratedServer::SummonMobs(Game::EntityTypeId type, const glm::dvec3& pos,
                                     int count, const SummonOptions& options,
                                     Game::DimensionId dimension, std::vector<int32_t>* outIds) {
        // The command source's level (CommandSourceStack.dimension) — a
        // /summon typed in the Nether, or run there by `/execute in`, lands
        // in the Nether. This used to be hard-wired to the Overworld.
        ServerLevel*       levelPtr = GetLevel(dimension);
        if (!levelPtr) return 0;
        ServerLevel&       level    = *levelPtr;
        MobManager*        mobs     = level.Mobs();
        ServerLevelBridge* mobLevel = level.MobLevel();
        if (!mobs || !mobLevel) return 0;

        int spawned = 0;
        Game::JavaRandom& rng = mobLevel->Random();

        for (int i = 0; i < count; ++i) {
            std::unique_ptr<Game::Mob> mob = MakeMobForLoad(type, mobLevel);
            if (!mob) break;

            mob->position = pos;
            if (count > 1) {
                // A ring of jitter rather than a point: identical positions
                // would leave every mob's collision resolution symmetric and
                // they would sit inside each other.
                mob->position.x += (rng.NextDouble() - 0.5) * 2.0;
                mob->position.z += (rng.NextDouble() - 0.5) * 2.0;
            }
            mob->yRot = rng.NextFloat() * 360.0f;
            mob->yHeadRot = mob->yRot;
            mob->yBodyRot = mob->yRot;

            // The two block-shaped entities need their own construction, not
            // just a position: a PrimedTnt summoned without it has no spawn hop
            // (so a stack of them sits in one column instead of scattering),
            // and a FallingBlockEntity needs the cell it is falling FROM for
            // the renderer's model-randomisation seed.
            //
            // MC does this through the summon's NBT; there is no NBT argument
            // here, so both take their vanilla defaults — a full 80-tick fuse,
            // and sand.
            if (auto* tnt = dynamic_cast<Game::PrimedTnt*>(mob.get())) {
                tnt->InitPrimed(mob->position, nullptr);
                // InitPrimed stamps the default 80-tick fuse, so any override
                // has to come after it. The step is what turns a stack of TNT
                // from one big blast into a sequence.
                if (options.tntFuse >= 0 || options.tntFuseStep != 0) {
                    const int base = options.tntFuse >= 0
                        ? options.tntFuse : Game::PrimedTnt::kDefaultFuse;
                    // Clamped at 1: a fuse of 0 or less detonates on the very
                    // first tick, before the client has even been told the
                    // entity exists, so the blast would have no visible source.
                    tnt->SetFuse(std::max(1, base + i * options.tntFuseStep));
                }
            } else if (auto* falling =
                           dynamic_cast<Game::FallingBlockEntity*>(mob.get())) {
                const glm::dvec3 at = mob->position;
                falling->InitFall(glm::ivec3(static_cast<int>(std::floor(at.x)),
                                             static_cast<int>(std::floor(at.y)),
                                             static_cast<int>(std::floor(at.z))),
                                  falling->CarriedState());
            }

            // MC SummonCommand calls finalizeSpawn whenever the summon carries
            // no NBT overriding it — which is every summon this engine has.
            mob->FinalizeSpawn(Game::SpawnReason::Command, nullptr);

            // Summoned mobs never despawn — MC does the same for /summon, and
            // it is what makes the command usable for testing at all.
            mob->SetPersistenceRequired(true);

            if (const int32_t id = mobs->Add(std::move(mob)); id != 0) {
                ++spawned;
                if (outIds) outIds->push_back(id);
            }
        }
        return spawned;
    }

    IntegratedServer::LineupResult IntegratedServer::SpawnMobLineup(
            const PlayerSession& session, const glm::dvec3& origin,
            double spacing, bool adults, bool babies) {
        LineupResult result;
        ServerLevel&       level    = LevelOf(session);
        MobManager*        mobs     = level.Mobs();
        ServerLevelBridge* mobLevel = level.MobLevel();
        if (!mobs || !mobLevel) return result;

        // Which types are MOBS: every non-Misc category, plus the four Misc
        // mob classes (golems, villager). The rest of Misc is projectiles,
        // block entities and the eye of ender — nothing to look at. The
        // dragon is skipped too: it exists only inside the End fight.
        const auto isLineupType = [](Game::EntityTypeId t) {
            if (t == Game::EntityTypeId::EnderDragon) return false;
            switch (t) {
                case Game::EntityTypeId::Villager:
                case Game::EntityTypeId::IronGolem:
                case Game::EntityTypeId::SnowGolem:
                case Game::EntityTypeId::CopperGolem:
                    return true;
                default:
                    return Game::GetEntityTypeInfo(t).category != Game::MobCategory::Misc;
            }
        };

        const auto place = [&](Game::EntityTypeId type, const glm::dvec3& at, bool baby) {
            std::unique_ptr<Game::Mob> mob = MakeMobForLoad(type, mobLevel);
            if (!mob) return false;
            mob->position = at;
            // Every one faces NORTH (-Z, yaw 180 in MC's +Z-clockwise
            // convention) and stays put: NoAI, as /summon {NoAI:1b} would,
            // so a tick step does not turn the heads to look around.
            const float yaw = 180.0f;
            mob->yRot = yaw;
            mob->xRot = 0.0f;
            mob->yHeadRot = yaw;
            mob->yBodyRot = yaw;
            mob->SetNoAi(true);
            // The same finalize /summon runs (variants, colours), then the
            // row's age is FORCED: an adult row must not roll a 5% baby
            // zombie, and the baby row only keeps types that can be one.
            mob->FinalizeSpawn(Game::SpawnReason::Command, nullptr);
            mob->SetBaby(baby);
            if (baby && !mob->IsBaby()) return false;
            mob->SetPersistenceRequired(true);
            return mobs->Add(std::move(mob)) != 0;
        };

        // A GRID, not one line: ~90 types in a row is 500+ blocks, which is
        // past every entity's tracking range and past the chunks the client
        // has, so a single row appears piecemeal as chunks stream and its
        // far end never at all. Twelve per row, CENTRED on the sender in
        // both axes: columns 2*spacing apart, each row's babies 1.5*spacing
        // in front of it, the next pair 2*spacing beyond that. At the
        // default 3 that is a 66 x ~75 block block, nothing farther than
        // ~50 blocks from the sender — inside even the fish's 64.
        constexpr int kPerRow = 12;
        const double colStep   = 2.0 * spacing;
        const double babyGap   = 1.5 * spacing;
        const double pairPitch = babyGap + 2.0 * spacing;
        int lineupTypes = 0;
        for (int i = 0; i < Game::kEntityTypeCount; ++i) {
            if (isLineupType(static_cast<Game::EntityTypeId>(i))) ++lineupTypes;
        }
        const int rows = (lineupTypes + kPerRow - 1) / kPerRow;
        const double z0 = origin.z - (rows - 1) * pairPitch * 0.5 - babyGap * 0.5;
        int column = 0;
        for (int i = 0; i < Game::kEntityTypeCount; ++i) {
            const auto type = static_cast<Game::EntityTypeId>(i);
            if (!isLineupType(type)) continue;
            const int col = column % kPerRow, row = column / kPerRow;
            const double x = origin.x - (kPerRow - 1) * colStep * 0.5 + col * colStep;
            const double zAdult = z0 + row * pairPitch;
            ++column;
            ++result.types;
            const bool gotAdult = adults && place(type, glm::dvec3(x, origin.y, zAdult), false);
            const bool gotBaby  = babies && place(type, glm::dvec3(x, origin.y, zAdult + babyGap), true);
            if (gotAdult) ++result.adults;
            if (gotBaby)  ++result.babies;
            Log::Info("[SpawnAll] #%d %s at row %d col %d: adult=%s baby=%s",
                      column - 1, std::string(Game::GetEntityTypeInfo(type).slug).c_str(),
                      row, col, gotAdult ? "yes" : (adults ? "FAILED" : "-"),
                      gotBaby ? "yes" : (babies ? "none" : "-"));
        }
        return result;
    }

    bool IntegratedServer::PlaceEndCrystalFromUse(PlayerSession& session,
                                                  const glm::ivec3& clicked) {
        // MC EndCrystalItem.useOn, transcribed: only on obsidian or bedrock,
        // only with an empty 1x2x1 above, only with no entity standing there.
        ServerLevel& level = LevelOf(session);
        Game::World* world = level.World();
        ServerLevelBridge* bridge = level.MobLevel();
        MobManager* mobs = level.Mobs();
        if (!world || !bridge || !mobs) return false;

        const Game::BlockID base = world->GetBlock(clicked.x, clicked.y, clicked.z);
        if (base != Game::BlockID::Obsidian && base != Game::BlockID::Bedrock) {
            return false;
        }

        const glm::ivec3 above = clicked + glm::ivec3(0, 1, 0);
        if (world->GetBlock(above.x, above.y, above.z) != Game::BlockID::Air) {
            return false;
        }

        Game::AABB box;
        box.min = glm::vec3(above);
        box.max = glm::vec3(above) + glm::vec3(1.0f, 2.0f, 1.0f);
        std::vector<Game::Entity*> occupants;
        bridge->GetEntitiesInBox(box, nullptr, occupants);
        if (!occupants.empty()) return false;
        std::vector<Game::LivingEntity*> players;
        bridge->GetPlayers(players);
        for (Game::LivingEntity* p : players) {
            if (p && p->GetAABB().Intersects(box)) return false;
        }

        auto crystal = std::make_unique<Game::EndCrystal>(bridge);
        crystal->position = glm::dvec3(above.x + 0.5, above.y, above.z + 0.5);
        // MC: a placed crystal has no bedrock base slab.
        crystal->SetShowBottom(false);
        if (mobs->Add(std::move(crystal)) == 0) return false;

        // MC: `if (dragonFight != null) dragonFight.tryRespawn()`.
        if (auto* fight = level.DragonFightController()) fight->TryRespawn();
        return true;
    }

    bool IntegratedServer::PlaceArmorStandFromUse(PlayerSession& session,
                                                  const Game::BlockHitResult& hit,
                                                  float playerYaw) {
        // MC ArmorStandItem.useOn, transcribed.
        if (hit.face == 0) return false;   // Direction.DOWN
        ServerLevel& level = LevelOf(session);
        Game::World* world = level.World();
        ServerLevelBridge* bridge = level.MobLevel();
        MobManager* mobs = level.Mobs();
        if (!world || !bridge || !mobs) return false;

        // BlockPlaceContext.getClickedPos: the clicked cell itself when its
        // block is replaceable (air, grass — no collision), else the cell
        // past the clicked face.
        const glm::ivec3 clicked = hit.blockPos;
        const Game::BlockID clickedId = world->GetBlock(clicked.x, clicked.y, clicked.z);
        glm::ivec3 pos = clicked;
        if (Game::BlockRegistry::HasCollision(clickedId)) {
            static constexpr glm::ivec3 kOffsets[6] = {
                {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
            if (hit.face >= 0 && hit.face < 6) pos = clicked + kOffsets[hit.face];
        }

        // The stand's box at the bottom centre of that cell: no block in it
        // (level.noCollision) and no entity in it (getEntities).
        const Game::EntityTypeInfo& info = Game::GetEntityTypeInfo(Game::EntityTypeId::ArmorStand);
        const glm::dvec3 feet(pos.x + 0.5, pos.y, pos.z + 0.5);
        Game::AABB box;
        box.min = glm::vec3(feet) - glm::vec3(info.width * 0.5f, 0.0f, info.width * 0.5f);
        box.max = glm::vec3(feet) + glm::vec3(info.width * 0.5f, info.height, info.width * 0.5f);
        {
            Game::PhysicsContext phys;
            phys.blockAccess = world;
            if (Game::CollidesAt(box, phys)) return false;
        }
        std::vector<Game::Entity*> occupants;
        bridge->GetEntitiesInBox(box, nullptr, occupants);
        if (!occupants.empty()) return false;
        std::vector<Game::LivingEntity*> players;
        bridge->GetPlayers(players);
        for (Game::LivingEntity* p : players) {
            if (p && p->GetAABB().Intersects(box)) return false;
        }

        auto stand = std::make_unique<Game::ArmorStand>(bridge);
        stand->position = feet;
        stand->oldPosition = feet;
        // snapTo(x, y, z, yRot, 0): the yaw snapped to the nearest 45°,
        // facing the player (rotation - 180).
        const float yRot = static_cast<float>(std::floor(
            (Game::Mth::WrapDegrees(playerYaw - 180.0f) + 22.5f) / 45.0f)) * 45.0f;
        stand->yRot = stand->yRotO = yRot;
        stand->yBodyRot = stand->yBodyRotO = yRot;
        stand->yHeadRot = stand->yHeadRotO = yRot;
        stand->xRot = stand->xRotO = 0.0f;
        const glm::dvec3 at = stand->position;
        if (mobs->Add(std::move(stand)) == 0) return false;
        // MC ArmorStandItem.useOn: level.playSound(null, x, y, z,
        // ARMOR_STAND_PLACE, BLOCKS, 0.75F, 0.8F).
        bridge->PlaySound(nullptr, at, Game::SoundEvents::ARMOR_STAND_PLACE, Game::SoundSource::Blocks, 0.75f, 0.8f);
        return true;
    }

    bool IntegratedServer::SpawnMobFromItemUse(Game::EntityTypeId type,
                                               const glm::ivec3& spawnPos,
                                               bool tryMoveDown, bool movedUp,
                                               Game::DimensionId dimension,
                                               int portalCooldownTicks,
                                               const std::function<void(Game::Mob&)>& configure) {
        // The level of the world that was clicked — the player's own, or
        // the one behind a portal they reached through. The caller chain
        // (ItemBehaviors -> Game::SpawnMobFromItem -> here) passes the
        // clicked world's dimension along.
        ServerLevel* clicked = GetLevel(dimension);
        ServerLevel&       level    = clicked ? *clicked : Overworld();
        MobManager*        mobs     = level.Mobs();
        ServerLevelBridge* mobLevel = level.MobLevel();
        Game::World*       world    = level.World();
        if (!mobs || !mobLevel || !world) return false;

        // MC SpawnEggItem.spawnMob's own gate, before anything is created.
        const Game::EntityTypeInfo& info = Game::GetEntityTypeInfo(type);
        if (info.notInPeaceful &&
            mobLevel->GetDifficulty() == Game::Difficulty::Peaceful) {
            return false;
        }

        std::unique_ptr<Game::Mob> mob = MakeMobForLoad(type, mobLevel);
        if (!mob) return false;

        // ── MC EntityType.create ────────────────────────────────────────────
        // The mob is provisionally placed one block ABOVE spawnPos and then
        // slid back down, so it comes to rest on the surface that was clicked
        // rather than inside it. Doing it the obvious way — spawn at spawnPos
        // and let gravity sort it out — puts a 1.95-block zombie's feet in the
        // block for a tick and lets the collision resolver eject it sideways.
        double yOffset = 0.0;
        if (tryMoveDown) {
            mob->position = glm::dvec3(spawnPos.x + 0.5, spawnPos.y + 1, spawnPos.z + 0.5);

            // MC getYOffset: collisions are gathered ONLY within the target
            // block's own cell (widened one block down when the click already
            // pushed the position up a face), so nothing outside that cell can
            // influence where the mob lands.
            Game::AABB region;
            region.min = glm::vec3(spawnPos.x, spawnPos.y - (movedUp ? 1.0f : 0.0f), spawnPos.z);
            region.max = glm::vec3(spawnPos.x + 1, spawnPos.y + 1, spawnPos.z + 1);

            Game::PhysicsContext phys;
            phys.blockAccess = world;
            std::vector<Game::AABBd> colliders;
            Game::CollectBlockColliders(Game::ToAABBd(region), phys, colliders);

            const double desired = movedUp ? -2.0 : -1.0;
            yOffset = 1.0 + Game::CollideAxis(1, Game::ToAABBd(mob->GetAABB()),
                                              desired, colliders);
        }

        mob->position = glm::dvec3(spawnPos.x + 0.5, spawnPos.y + yOffset, spawnPos.z + 0.5);
        mob->yRot = Game::Mth::WrapDegrees(mobLevel->Random().NextFloat() * 360.0f);
        mob->xRot = 0.0f;
        mob->yHeadRot = mob->yRot;
        mob->yBodyRot = mob->yRot;

        // MC EntityType.create runs finalizeSpawn for SPAWN_ITEM_USE too, which
        // is why a vanilla spawn egg can produce a grey, brown or (rarely) pink
        // sheep rather than always a white one.
        mob->FinalizeSpawn(Game::SpawnReason::SpawnItemUse, nullptr);
        // MobBucketItem.spawn: loadFromBucketTag after finalizeSpawn, before add.
        if (configure) configure(*mob);

        // MC does NOT mark egg-spawned mobs persistent — they despawn like any
        // natural spawn. /summon is the one that pins them (see SummonMobs).
        // A mob born IN a portal (the nether portal's zombified piglin) is
        // held out of it for a while — MC Entity.setPortalCooldown on the
        // spawn — or it would step through on its first wander.
        if (portalCooldownTicks > 0) mob->portal.SetCooldown(portalCooldownTicks);
        return mobs->Add(std::move(mob)) != 0;
    }

    // ── Wither summoning ritual ─────────────────────────────────────────────
    //
    // Port of MC WitherSkullBlock.checkSpawn plus the slice of
    // BlockPattern/BlockPatternBuilder it depends on. The full pattern is one
    // 3-wide 3-tall aisle,
    //
    //     "^^^"      ^ = wither skeleton skull block, floor OR wall variant,
    //     "###"          any rotation/facing (BlockStatePredicate.forBlock
    //     "~#~"          matches the BLOCK, ignoring properties)
    //                # = BlockTags.WITHER_SUMMON_BASE_BLOCKS = soul sand
    //                    or soul soil
    //                ~ = blockState.isAir() — MUST be air, not "anything"
    //
    // and BlockPattern.find tries it with every (forwards, up ⊥ forwards)
    // direction pair at every position of a 3³ probe cube — which is why in
    // vanilla the T can face any of the four ways AND be built lying flat, and
    // why any of the three skulls can be the one placed last. All of that
    // falls out of porting find() literally instead of scanning two axes.
    namespace {
        // The six Direction unit vectors, MC ordinal order (D U N S W E).
        constexpr glm::ivec3 kDirSteps[6] = {
            {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0},
        };

        glm::ivec3 IntCross(const glm::ivec3& a, const glm::ivec3& b) {
            return { a.y * b.z - a.z * b.y,
                     a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x };
        }

        // MC BlockPattern.translateAndRotate: right/down/forwards pattern
        // coordinates → world offset from the front-top-left corner.
        glm::ivec3 PatternCell(const glm::ivec3& frontTopLeft,
                               const glm::ivec3& forwards, const glm::ivec3& up,
                               int right, int down, int fwd) {
            const glm::ivec3 rightVec = IntCross(forwards, up);
            return frontTopLeft + up * (-down) + rightVec * right + forwards * fwd;
        }

        bool IsWitherSkullBlock(Game::BlockID id) {
            return id == Game::BlockID::WitherSkeletonSkull ||
                   id == Game::BlockID::WitherSkeletonWallSkull;
        }
        // BlockTags.WITHER_SUMMON_BASE_BLOCKS (data tag): soul_sand, soul_soil.
        bool IsWitherSummonBase(Game::BlockID id) {
            return id == Game::BlockID::SoulSand || id == Game::BlockID::SoulSoil;
        }

        // pattern[y][x] for the single aisle above. 0 = '^', 1 = '#', 2 = '~'.
        constexpr int kWitherPattern[3][3] = {
            { 0, 0, 0 },
            { 1, 1, 1 },
            { 2, 1, 2 },
        };

        bool WitherCellMatches(Game::World& world, const glm::ivec3& cell, int predicate) {
            const Game::BlockID id = world.GetBlock(cell.x, cell.y, cell.z);
            switch (predicate) {
                case 0:  return IsWitherSkullBlock(id);
                case 1:  return IsWitherSummonBase(id);
                default: return id == Game::BlockID::Air;   // MC state.isAir()
            }
        }

        bool WitherPatternMatches(Game::World& world, const glm::ivec3& frontTopLeft,
                                  const glm::ivec3& forwards, const glm::ivec3& up) {
            for (int x = 0; x < 3; ++x) {
                for (int y = 0; y < 3; ++y) {
                    const glm::ivec3 cell = PatternCell(frontTopLeft, forwards, up, x, y, 0);
                    if (!WitherCellMatches(world, cell, kWitherPattern[y][x])) return false;
                }
            }
            return true;
        }
    } // namespace

    void IntegratedServer::CheckWitherSpawn(const glm::ivec3& pos) {
        // OVERWORLD. PlayerSession calls this with a block position only, and
        // its whole block-interaction path still reads GetWorld() (the
        // overworld) too — so pinning here keeps the ritual consistent with the
        // world the placement itself went into. Building the T in the Nether
        // needs that path to carry the session's dimension first.
        ServerLevel&       level    = Overworld();
        MobManager*        mobs     = level.Mobs();
        ServerLevelBridge* mobLevel = level.MobLevel();
        Game::World*       world    = level.World();
        if (!mobs || !mobLevel || !world) {
            Log::Warning("[WitherRitual] skull placed at (%d,%d,%d) but the "
                         "server has no mobs/level/world — no check ran",
                         pos.x, pos.y, pos.z);
            return;
        }

        // MC checkSpawn's gates: server side (this whole class is), the placed
        // block IS a wither skull (the caller guarantees it), the position is
        // in the world, and not peaceful.
        if (pos.y < Game::World::MIN_Y) return;
        if (mobLevel->GetDifficulty() == Game::Difficulty::Peaceful) {
            Log::Debug("[WitherRitual] skull placed but difficulty is Peaceful");
            return;
        }
        Log::Debug("[WitherRitual] checking pattern around (%d,%d,%d)",
                   pos.x, pos.y, pos.z);

        // MC BlockPattern.find: probe every frontTopLeft in the 3³ cube at
        // `pos` against every valid (forwards, up) pair.
        glm::ivec3 frontTopLeft{}, forwards{}, up{};
        bool found = false;
        for (int dx = 0; dx < 3 && !found; ++dx)
        for (int dy = 0; dy < 3 && !found; ++dy)
        for (int dz = 0; dz < 3 && !found; ++dz) {
            const glm::ivec3 origin = pos + glm::ivec3(dx, dy, dz);
            for (int f = 0; f < 6 && !found; ++f) {
                for (int u = 0; u < 6; ++u) {
                    const glm::ivec3 fv = kDirSteps[f];
                    const glm::ivec3 uv = kDirSteps[u];
                    if (uv == fv || uv == -fv) continue;   // up must be ⊥ forwards
                    if (WitherPatternMatches(*world, origin, fv, uv)) {
                        frontTopLeft = origin; forwards = fv; up = uv;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            // Diagnostic dump (Debug — skulls get placed as decoration): the
            // vertical slice through the placed skull on both horizontal
            // axes. The most common legitimate miss is MC's own pit rule —
            // the '~' cells flanking the BASE soul sand must be AIR
            // (WitherSkullBlock.java:90, state.isAir()), so a T flush with
            // the ground, or with grass plants beside the base, refuses in
            // vanilla too.
            for (int dy = 0; dy >= -2; --dy) {
                std::string rowX, rowZ;
                for (int d = -2; d <= 2; ++d) {
                    rowX += std::to_string(static_cast<int>(
                                world->GetBlock(pos.x + d, pos.y + dy, pos.z))) + " ";
                    rowZ += std::to_string(static_cast<int>(
                                world->GetBlock(pos.x, pos.y + dy, pos.z + d))) + " ";
                }
                Log::Debug("[WitherRitual] y%+d  x-slice: %s | z-slice: %s",
                           dy, rowX.c_str(), rowZ.c_str());
            }
            return;
        }
        Log::Info("[WitherRitual] pattern matched — summoning the wither");

        std::unique_ptr<Game::Mob> wither = MakeMobForLoad(Game::EntityTypeId::Wither, mobLevel);
        if (!wither) return;

        // CarvedPumpkinBlock.clearPatternBlocks: every pattern cell — the two
        // air corners included — becomes air with MC flag 2 (send to clients,
        // NO neighbour updates yet; those come after the boss exists, below).
        // MC also fires levelEvent 2001 per cell (break particles + sound):
        // the sound half plays here (Game::PlayLevelEventSound); the particles
        // have no level-event channel yet.
        constexpr uint32_t kClearFlags = Game::World::UpdateFlags::UpdateShapes |
                                         Game::World::UpdateFlags::RecomputeLight |
                                         Game::World::UpdateFlags::UpdateHeightmap |
                                         Game::World::UpdateFlags::MarkDirty;
        for (int x = 0; x < 3; ++x) {
            for (int y = 0; y < 3; ++y) {
                const glm::ivec3 cell = PatternCell(frontTopLeft, forwards, up, x, y, 0);
                const Game::BlockState was = world->GetBlockState(cell.x, cell.y, cell.z);
                world->SetBlock(cell.x, cell.y, cell.z, Game::BlockID::Air, kClearFlags);
                Game::PlayLevelEventSound(*world, nullptr, Game::LevelEvent::PARTICLES_DESTROY_BLOCK, cell,
                                          static_cast<int>(was.RawId()), world->Random());
            }
        }

        // MC: spawn at getBlock(1, 2, 0) — the centre of the pattern's bottom
        // row (the soul-sand column base) — with the wither snapped to
        // (+0.5, +0.55, +0.5) and yawed along the pattern plane:
        //   forwards axis == X ? 0° : 90°, body rotation matching.
        const glm::ivec3 spawnCell = PatternCell(frontTopLeft, forwards, up, 1, 2, 0);
        const float yaw = (forwards.x != 0) ? 0.0f : 90.0f;
        wither->position = glm::dvec3(spawnCell.x + 0.5, spawnCell.y + 0.55,
                                      spawnCell.z + 0.5);
        wither->yRot = wither->yBodyRot = wither->yHeadRot = yaw;
        wither->xRot = 0.0f;

        // MC: wither.makeInvulnerable() — the ritual is the ONE spawn that
        // charges up (blue armour, 220 ticks, the spawn explosion at the
        // end); /summon and the egg do not.
        wither->FinalizeSpawn(Game::SpawnReason::Triggered, nullptr);
        if (auto* boss = dynamic_cast<Game::Wither*>(wither.get())) boss->MakeInvulnerable();

        // MC: CriteriaTriggers.SUMMONED_ENTITY for every player within 50
        // blocks — no advancement system here, skipped. (Vanilla's piglin
        // anger applies to golem construction, not this ritual.)

        Log::Info("[Server] Wither summoned at (%d,%d,%d)",
                  spawnCell.x, spawnCell.y, spawnCell.z);
        mobs->Add(std::move(wither));

        // CarvedPumpkinBlock.updatePatternBlocks: NOW run the deferred
        // neighbour updates for every cleared cell.
        for (int x = 0; x < 3; ++x) {
            for (int y = 0; y < 3; ++y) {
                const glm::ivec3 cell = PatternCell(frontTopLeft, forwards, up, x, y, 0);
                world->NotifyNeighborBlocks(cell.x, cell.y, cell.z);
            }
        }
    }

    namespace {
        // MC SpawnEggItem.spawnOffspringFromSpawnEgg, reached from
        // Mob.checkAndHandleImportantInteractions: a spawn egg used ON a live
        // mob of the egg's own type makes a BABY of it at the parent's feet.
        // An ageable parent breeds it (getBreedOffspring — variants, colour
        // and so on inherit); anything else is created fresh and flagged. A
        // type with no baby form (creeper) stays adult, so nothing spawns and
        // the click passes through to mobInteract. Consumes one egg on
        // success (creative is restored by the caller's snapshot).
        //
        // Not ported: applyComponentsFromItemStack (no item components carry
        // entity data here) and Fox.onOffspringSpawnedFromEgg's trust of the
        // spawner (fox trust is not modelled).
        Game::UseResult SpawnOffspringFromSpawnEgg(Game::Mob& parent, Game::ItemStack& held) {
            if (held.IsEmpty()) return Game::UseResult::Pass;
            const Game::EntityTypeId eggType = Game::SpawnEggEntityType(held.itemId);
            if (eggType == Game::EntityTypeId::Count) return Game::UseResult::Pass;
            // Mob.interact's own guard, then spawnsEntity(type).
            if (!parent.IsAlive() || eggType != parent.GetType()) return Game::UseResult::Pass;
            Game::EntityLevel* level = parent.Level();
            if (!level) return Game::UseResult::Pass;

            std::unique_ptr<Game::Mob> offspring;
            if (auto* ageable = dynamic_cast<Game::Animal*>(&parent)) {
                offspring = ageable->CreateBaby();
            } else {
                offspring = MakeMobForLoad(eggType, level);
            }
            if (!offspring) return Game::UseResult::Pass;

            offspring->SetBaby(true);
            if (!offspring->IsBaby()) return Game::UseResult::Pass;

            // snapTo(pos, 0, 0) — the parent's position, facing +Z.
            offspring->position = parent.position;
            offspring->yRot = 0.0f;
            offspring->xRot = 0.0f;
            offspring->yHeadRot = 0.0f;
            offspring->yBodyRot = 0.0f;
            level->AddFreshEntity(std::move(offspring));

            held.count -= 1;
            if (held.count <= 0) held.Clear();
            return Game::UseResult::Success;
        }
    } // namespace

    void IntegratedServer::HandleInteract(uint32_t connectionId, int32_t entityId,
                                          bool attack, bool sprinting,
                                          int dragonPart, const glm::vec3* location) {
        if (!m_sessionManager) return;

        // Everything below — the attacker's view, the target's id, the mob
        // manager it is looked up in — belongs to the level the ATTACKER is
        // standing in. Entity ids are allocated per level, so resolving the
        // wrong one would hit a same-numbered mob in another world.
        auto session = m_sessionManager->GetSessionByConnection(connectionId);
        if (!session) return;
        ServerLevel&       level    = LevelOf(*session);
        MobManager*        mobs     = level.Mobs();
        ServerLevelBridge* mobLevel = level.MobLevel();
        if (!mobs || !mobLevel) return;

        Server::PlayerEntityView* attacker = mobLevel->GetPlayerView(connectionId);
        if (!attacker || attacker->IsSpectator()) return;

        // The id space already separates the two (see Game::kMobEntityIdBase):
        // players are connection ids below kItemEntityIdBase, mobs above
        // kMobEntityIdBase. Dispatching on it is what lets one packet carry
        // both PvE and PvP without a kind byte.
        Game::LivingEntity* target = nullptr;
        Game::Mob*          mobTarget = nullptr;
        // A mob reached THROUGH a portal lives in another level. Ids are
        // process-wide, so it is found by search; the attacker then acts
        // from their image through the nearest portal into that level —
        // reach, knockback direction and everything else read the view's
        // position, which is moved for the duration of the hit and put
        // back after. The mob keeps no reference to a view of another
        // level (it would dangle when that player leaves), so a hit does
        // not make it retaliate across the portal; the chase logic in
        // EntityPortalTravel is what follows a player through.
        struct AttackerImage {
            Server::PlayerEntityView* view = nullptr;
            glm::dvec3 position{0.0}, oldPosition{0.0};
            float yRot = 0.0f;
            Game::Mob* mob = nullptr;
            ~AttackerImage() {
                if (!view) return;
                if (mob) mob->ClearReferenceTo(view);
                view->position = position; view->oldPosition = oldPosition; view->yRot = yRot;
            }
        } image;
        if (Game::IsMobEntityId(entityId)) {
            mobTarget = mobs->Find(entityId);
#if ENABLE_IMMERSIVE_PORTALS
            if (m_immersivePortals) {
                // The level the mob is in: the attacker's own, or another.
                ServerLevel* targetLevel = mobTarget ? &level : nullptr;
                if (!targetLevel) {
                    ForEachLevel([&](ServerLevel& other) {
                        if (targetLevel || &other == &level || !other.Mobs() || !other.MobLevel()) return;
                        if (other.Mobs()->Find(entityId)) targetLevel = &other;
                    });
                }
                Game::Mob* found = targetLevel ? targetLevel->Mobs()->Find(entityId) : nullptr;
                if (found) {
                    // Out of direct reach (or in another level altogether):
                    // the portal that brings the attacker's image closest to
                    // the mob is the one they are hitting through.
                    constexpr double kDirectRangeSq = 6.0 * 6.0;
                    const glm::dvec3 eye = attacker->GetEyePosition();
                    const bool direct = (targetLevel == &level) &&
                        found->GetAABBd().DistanceToSqr(eye) < kDirectRangeSq;
                    if (!direct) {
                        const Game::Immersive::Portal* via = nullptr;
                        double best = 1e30;
                        const glm::dvec3 mobCentre = found->position + glm::dvec3(0.0, 0.5 * found->GetBbHeight(), 0.0);
                        for (const Game::Immersive::Portal* p :
                             m_immersivePortals->CollectNear(level.Dimension(), eye, 6.0)) {
                            if (p->IsMirror() || p->destDimension != targetLevel->Dimension()) continue;
                            if (!p->Has(Game::Immersive::PortalFlag::Interactable)) continue;
                            const double d = glm::length(p->TransformPoint(eye) - mobCentre);
                            if (d < best) { best = d; via = p; }
                        }
                        if (!via && targetLevel != &level) return;   // no surface into that level
                        if (via) {
                            image.view = attacker;
                            image.position = attacker->position;
                            image.oldPosition = attacker->oldPosition;
                            image.yRot = attacker->yRot;
                            attacker->position    = via->TransformPoint(attacker->position);
                            attacker->oldPosition = via->TransformPoint(attacker->oldPosition);
                            {
                                const glm::vec3 fwd = Game::Mth::ViewVector(0.0f, attacker->yRot);
                                attacker->yRot = Game::Mth::YRotFromVector(
                                    glm::vec3(via->TransformLocalVecNonScale(glm::dvec3(fwd))));
                            }
                            // Only a view of ANOTHER level must not be remembered
                            // by the mob; in its own level the view is a normal target.
                            if (targetLevel != &level) image.mob = found;
                        }
                    }
                    mobs      = targetLevel->Mobs();
                    mobLevel  = targetLevel->MobLevel();
                    mobTarget = found;
                }
            }
#endif
            target = mobTarget;
        } else if (entityId >= 0 && entityId < Game::kItemEntityIdBase) {
            // A player. Never yourself: MC's pick never returns the attacker,
            // but a hand-made packet could.
            if (static_cast<uint32_t>(entityId) == connectionId) return;
            target = mobLevel->GetPlayerView(static_cast<uint32_t>(entityId));
        }
        if (!target || !target->IsAlive()) return;

        // MC Player.cannotAttack -> Entity.isAttackable. Creative and spectator
        // players are not valid targets.
        if (!target->IsAttackable()) return;

        // Reach check, server-side. The client picks the target, but a client
        // that picked one 40 blocks away must not get to hit it.
        //
        // MC ServerboundInteractPacket.isWithinRange (:82) ->
        // Player.isWithinEntityInteractionRange (Player.java:1905):
        //
        //     double maxRange = this.entityInteractionRange() + buffer;
        //     return aabb.distanceToSqr(this.getEyePosition()) < maxRange * maxRange;
        //
        // ENTITY_INTERACTION_RANGE defaults to 3.0 and handleInteract passes a
        // buffer of 3.0, so the magnitude matches the 6.0 we already used — but
        // MC measures from the player's EYE to the nearest point on the
        // target's bounding box, not centre to centre. Centre-to-centre makes
        // tall or wide mobs read as further away than they are, so a legitimate
        // hit on a spider's flank or an enderman's legs got rejected.
        constexpr double kEntityInteractionRange = 3.0;   // Attributes.ENTITY_INTERACTION_RANGE
        constexpr double kReachBuffer            = 3.0;   // handleInteract's slack
        constexpr double kMaxRangeSq =
            (kEntityInteractionRange + kReachBuffer) * (kEntityInteractionRange + kReachBuffer);

        // ── Ender dragon: the PART is the target (MC EnderDragonPart) ──────
        //
        // MC's interact packet names a part entity and every check runs
        // against that part's own box; the dragon body itself is not
        // pickable. Here the part index rides the packet, so: require one,
        // clamp it, recompute the server's own part layout, and measure
        // reach against THAT box — the 16-block body box must not validate a
        // click vanilla's parts would reject.
        auto* dragonTarget = dynamic_cast<Game::EnderDragon*>(mobTarget);
        bool dragonHeadHit = false;
        if (dragonTarget) {
            if (dragonPart < 0 ||
                dragonPart >= Game::EnderDragon::kDragonPartCount) {
                return;   // no part claimed — vanilla has nothing to hit either
            }
            Game::AABB parts[Game::EnderDragon::kDragonPartCount];
            dragonTarget->ComputePartBoxes(parts);
            const Game::AABBd partBox = Game::ToAABBd(parts[dragonPart]);
            if (partBox.DistanceToSqr(attacker->GetEyePosition()) >= kMaxRangeSq) {
                return;
            }
            dragonHeadHit = (dragonPart == Game::EnderDragon::kDragonPartHead);
        } else if (target->GetAABBd().DistanceToSqr(attacker->GetEyePosition()) >=
                   kMaxRangeSq) {
            return;
        }

        // ── INTERACT (right-click) ─────────────────────────────────────────
        // MC Player.interactOn, in ITS order:
        //
        //   1. entity.interact(player, hand)          -> Mob.mobInteract
        //   2. if that did not consume, and the stack is non-empty:
        //      itemStack.interactLivingEntity(player, entity, hand)
        //
        // The ENTITY goes first. That is what lets a sheep claim shears in its
        // own mobInteract while dye — which the sheep passes on — reaches the
        // item hook underneath.
        if (!attack) {
            if (!mobTarget) {
                // A player is no mob, but a SHEEP MORPH takes shears like
                // one (Sheep.mobInteract → shear): wool drops, the morph is
                // sheared until it grazes.
                auto* view = dynamic_cast<PlayerEntityView*>(target);
                ServerPlayer* victim = view ? view->GetPlayer() : nullptr;
                ServerPlayer* player = attacker->GetPlayer();
                if (!victim || !player) return;
                const uint32_t code = victim->getMorph();
                // A DOOR morph swings on a right-click, as a door does
                // (DoorBlock.useWithoutItem: open toggles).
                if (Game::Morph::IsValid(code) && Game::Morph::KindOf(code) == Game::Morph::Kind::Block &&
                    Game::IsDoorBlock(static_cast<Game::BlockID>(Game::Morph::BlockOf(code)))) {
                    victim->setMorph(Game::Morph::WithBlockOpen(code, !Game::Morph::IsBlockOpen(code)),
                                     victim->getMorphSpeed());
                    if (auto victimSession = m_sessionManager->GetSession(victim->getPlayerId())) {
                        if (auto* vc = victimSession->GetConnection()) vc->SendPlayerAbilities(*victim);
                    }
                    return;
                }
                // A SNOW GOLEM morph loses its pumpkin to shears
                // (SnowGolem.shear: the carved pumpkin drops).
                if (Game::Morph::IsMob(code, Game::EntityTypeId::SnowGolem) && !Game::Morph::IsSheared(code)) {
                    const int slot = player->getInventory().GetSelectedSlot();
                    const Game::ItemStack& held = player->getInventory().GetSlot(Game::Inventory::HotbarToIndex(slot));
                    if (held.itemId != Game::Items::Shears) return;
                    victim->setMorph(Game::Morph::WithSheared(code, true), victim->getMorphSpeed());
                    mobLevel->SpawnItemDrop(victim->getPosition() + glm::dvec3(0.0, 1.0, 0.0),
                                            Game::ItemRegistry::FromBlock(Game::BlockID::CarvedPumpkin), 1);
                    if (auto victimSession = m_sessionManager->GetSession(victim->getPlayerId())) {
                        if (auto* vc = victimSession->GetConnection()) vc->SendPlayerAbilities(*victim);
                    }
                    return;
                }
                if (!Game::Morph::IsMob(code, Game::EntityTypeId::Sheep) || Game::Morph::IsSheared(code)) return;
                const int slot = player->getInventory().GetSelectedSlot();
                const Game::ItemStack& held = player->getInventory().GetSlot(Game::Inventory::HotbarToIndex(slot));
                if (held.itemId != Game::Items::Shears) return;
                victim->setMorph(Game::Morph::WithSheared(code, true), victim->getMorphSpeed());
                const int rolls = 1 + mobLevel->Random().NextInt(3);
                for (int i = 0; i < rolls; ++i) {
                    mobLevel->SpawnItemDrop(victim->getPosition() + glm::dvec3(0.0, 1.0, 0.0),
                                            Game::Sheep::WoolItemForColor(0), 1);
                }
                if (auto victimSession = m_sessionManager->GetSession(victim->getPlayerId())) {
                    if (auto* vc = victimSession->GetConnection()) vc->SendPlayerAbilities(*victim);
                }
                return;
            }
            ServerPlayer* player = attacker->GetPlayer();
            if (!player) return;

            const int slot = player->getInventory().GetSelectedSlot();
            Game::ItemStack& held = player->getInventory().MutableSlot(
                Game::Inventory::HotbarToIndex(slot));

            const bool creative = (player->getGameMode() == GameMode::CREATIVE);

            // An armor stand's own interact (MC ArmorStand.interact): the
            // swap writes the hand itself — in creative too, where a piece
            // taken off the stand must land in the empty hand rather than
            // be "restored" away by the generic creative restore below.
            if (auto* stand = dynamic_cast<Game::ArmorStand*>(mobTarget)) {
                const Game::UseResult r = stand->Interact(*attacker, held,
                                                          location ? *location : glm::vec3(0.0f));
                if (Game::ConsumesAction(r) || r == Game::UseResult::Fail) {
                    session->SendInventoryFull();
                    return;
                }
            }

            const Game::ItemStack before = held;

            // MC restores only the COUNT in creative — see the same note on
            // the block-use path in PlayerSession::HandleUseItemOn, where
            // restoring the whole stack silently wiped a component.
            const auto restoreCreative = [&] {
                if (!creative) return;
                if (held.IsEmpty()) held = before;
                else                held.count = before.count;
            };

            // MC Mob.interact → checkAndHandleImportantInteractions: the
            // spawn-egg branch runs BEFORE mobInteract, so an egg on its own
            // adult always makes a baby rather than, say, feeding it.
            Game::UseResult r = SpawnOffspringFromSpawnEgg(*mobTarget, held);
            if (!Game::ConsumesAction(r)) r = mobTarget->MobInteract(*attacker, held);
            // A villager's trading screen (Merchant.openTradingScreen) is a
            // request the mob recorded on the player — open it now, on the
            // same round as the interaction.
            session->FlushPendingMenuOpen();

            if (!Game::ConsumesAction(r) && !held.IsEmpty()) {
                const Game::Item& item = Game::ItemRegistry::Get(held.itemId);
                if (item.interactLivingEntity) {
                    r = item.interactLivingEntity(held, *target);
                }
            }

            restoreCreative();

            if (Game::ConsumesAction(r)) {
                // The stack may have shrunk and the mob's synched data changed.
                // The tracker picks the mob up on its own; the inventory has to
                // be pushed.
                session->SendInventoryFull();
            }
            return;
        }

        // ── ATTACK — MC Player.attack, in its order ────────────────────────
        ServerPlayer* player = attacker->GetPlayer();
        if (!player) return;

        // The swing MC's client sends right after the attack
        // (ServerboundSwingPacket -> Player.swing): there is no swing packet
        // here, so the attack is where the server learns of it. Landed or
        // not. See PlayerEntityView::SetDigging.
        attacker->Swing();

        const Game::ItemStack& held = player->getInventory().GetSlot(
            Game::Inventory::HotbarToIndex(player->getInventory().GetSelectedSlot()));

        float itemDamage = 0.0f, itemSpeed = 0.0f;
        Game::GetItemAttackAttributes(held.itemId, itemDamage, itemSpeed);

        // MC reads ATTACK_DAMAGE, which is the player's base plus the held
        // item's main-hand modifier (a bare hand is 1.0), then STRENGTH's
        // +3 and WEAKNESS's -4 per level.
        float damage = player->getAttackDamage(itemDamage);

        // MC getAttackStrengthScale(0.5F) — the half tick is MC's, and it is
        // why a hit that lands exactly on the boundary counts as full strength.
        const float strengthScale = player->getAttackStrengthScale(0.5f);

        // MC baseDamageScaleFactor: 0.2 + scale^2 * 0.8. Swinging at zero
        // charge does 20% damage, not zero — the curve is quadratic, so half a
        // bar is 40%, not 60%.
        damage *= 0.2f + strengthScale * strengthScale * 0.8f;

        // MC onAttack -> resetAttackStrengthTicker. Reset BEFORE the hit lands
        // so a second click in the same tick is already at zero charge.
        player->resetAttackStrengthTicker();

        if (damage <= 0.0f) return;

        const bool fullStrength = strengthScale > 0.9f;

        // MC Player.playServerSideSound: level.playSound(null, player x/y/z,
        // sound, PLAYERS, 1, 1) — everyone nearby hears the swing, the
        // attacker included (the client does not predict attacks).
        const auto playServerSideSound = [attacker](const char* event) {
            if (Game::EntityLevel* lvl = attacker->Level()) {
                lvl->PlaySound(nullptr, attacker->position, event, Game::SoundSource::Players, 1.0f, 1.0f);
            }
        };
        // MC: a sprinting full-strength hit is the knockback attack.
        if (sprinting && fullStrength) playServerSideSound(Game::SoundEvents::PLAYER_ATTACK_KNOCKBACK);

        // MC Player.canCriticalAttack — every term of it:
        //   fallDistance > 0, !onGround, !onClimbable, !isInWater,
        //   !isMobilityRestricted, !isPassenger, target is a LivingEntity,
        //   !isSprinting.
        //
        // fallDistance > 0 && !onGround is why a crit is a hit taken on the way
        // DOWN — the accumulator only grows while descending (see
        // PlayerSession::UpdateMovementStats). isMobilityRestricted is
        // BLINDNESS (Player.isMobilityRestricted); isPassenger has no analogue
        // — no vehicles exist.
        const bool crit = fullStrength
                       && player->getFallDistance() > 0.0f
                       && !player->isOnGround()
                       && !IsOnClimbable(*player)
                       && !attacker->IsInWater()
                       && !player->isMobilityRestricted()
                       && !sprinting;
        if (crit) damage *= 1.5f;

        // MC isSweepAttack: a full-strength, non-crit, non-knockback hit with a
        // sword while standing on the ground and moving no faster than a walk.
        // The speed gate (`< getSpeed() * 2.5`) is what stops a sprint-jump
        // landing from sweeping; getSpeed() is the player's walk speed, 0.1
        // blocks/tick, so the cutoff is 0.25 per tick.
        constexpr double kMaxSweepSpeed = 0.1 * 2.5;
        const bool sweep = fullStrength && !crit && !sprinting
                        && player->isOnGround()
                        && player->getKnownHorizontalMovement() < kMaxSweepSpeed
                        && Game::IsSwordItem(held.itemId);

        // MC Player.attack reaches the dragon through the part entity's
        // hurtServer -> EnderDragon.hurt(part, ...): a head hit is full
        // damage, everything else the body reduction (HurtPart's default).
        const bool hit = dragonTarget
            ? dragonTarget->HurtPart(Game::MobDamageSource::PlayerAttack, damage,
                                     attacker, dragonHeadHit,
                                     /*direct=*/attacker)
            : target->Hurt(Game::MobDamageSource::PlayerAttack, damage, attacker);
        if (hit) {
            attacker->SetLastHurtMob(target);

            // The Hush's echo blade (docs/the-hush.md): a landed hit "silences"
            // the target — Slowness II for 3 s (60 ticks). Any living target:
            // a player is reached through its PlayerEntityView, whose effect
            // list IS the ServerPlayer's (ticked, synced, saved). Stacks
            // through LivingEntity::AddEffect's MC update rules, so a second
            // hit refreshes the 3 s rather than doubling it.
            if (target && held.itemId == Game::Items::EchoBlade) {
                constexpr int kSilenceTicks = 60;
                target->AddEffect(Game::MobEffectInstance(Game::MobEffectId::Slowness,
                                                          kSilenceTicks, /*amp=*/1),
                                  attacker);
            }

            // Twilight Forest's fiery sword (FierySwordItem.hurtEnemy): a
            // landed hit sets a non-fire-immune target alight for 15 s.
            // Mob targets only, for the reason the echo blade gives above.
            if (mobTarget && held.itemId == Game::Items::FierySword && !mobTarget->FireImmune()) {
                mobTarget->IgniteForSeconds(15);
            }

            // MC's sprint hit adds 0.5 extra knockback on top of the base the
            // damage already applied (Player.causeExtraKnockback).
            //
            // The (sin, -cos) is MC's and is NOT the facing vector — it is the
            // facing NEGATED, because Knockback subtracts the impulse it is
            // handed (that is what makes Hurt's "push away from the attacker"
            // read as attacker-minus-target). Passing the facing directly, as
            // this did, dragged the target toward the attacker on every
            // sprint hit instead of launching it.
            if (sprinting && fullStrength) {
                const float yaw = attacker->yRot * Game::Mth::kDegToRad;
                target->Knockback(0.5, std::sin(yaw), -std::cos(yaw));
            }
            if (sweep) {
                // MC Player.doSweepAttack opens with PLAYER_ATTACK_SWEEP.
                playServerSideSound(Game::SoundEvents::PLAYER_ATTACK_SWEEP);
                DoSweepAttack(*attacker, *target, strengthScale);
            }
            // MC attackVisualEffects: the crit sound, or — for a plain hit —
            // the strong / weak swing.
            if (crit) playServerSideSound(Game::SoundEvents::PLAYER_ATTACK_CRIT);
            if (!crit && !sweep) {
                playServerSideSound(fullStrength ? Game::SoundEvents::PLAYER_ATTACK_STRONG
                                                 : Game::SoundEvents::PLAYER_ATTACK_WEAK);
            }
            BroadcastAttackEffects(level.Dimension(), *target, crit);

            // MC Player.attack's last line: 0.1 exhaustion per landed hit.
            // causeFoodExhaustion no-ops for an invulnerable player, which is
            // how creative and spectator are excluded — the same gate the
            // movement exhaustion sources use in PlayerSession.
            const GameMode mode = player->getGameMode();
            if (mode == GameMode::SURVIVAL || mode == GameMode::ADVENTURE) {
                player->getFoodData().addExhaustion(0.1f);
            }
        } else {
            // MC: the target shrugged it off.
            playServerSideSound(Game::SoundEvents::PLAYER_ATTACK_NODAMAGE);
        }
    }

    bool IntegratedServer::IsOnClimbable(const ServerPlayer& player) const {
        // The blocks under this player's feet are the ones in the world they
        // are standing in — read from the player's own dimension rather than
        // the overworld, or a ladder in the Nether would never register.
        ServerLevel* level = GetLevel(Game::DimensionFromRaw(player.getDimensionId()));
        Game::World* world = level ? level->World() : nullptr;
        if (!world) return false;
        const glm::dvec3 pos = player.getPosition();
        const Game::BlockID block = world->GetBlock(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z)));

        // MC BlockTags.CLIMBABLE (data/minecraft/tags/block/climbable.json).
        switch (block) {
            case Game::BlockID::Ladder:
            case Game::BlockID::Vine:
            case Game::BlockID::Scaffolding:
            case Game::BlockID::WeepingVines:
            case Game::BlockID::WeepingVinesPlant:
            case Game::BlockID::TwistingVines:
            case Game::BlockID::TwistingVinesPlant:
            case Game::BlockID::CaveVines:
            case Game::BlockID::CaveVinesPlant:
                return true;
            default:
                return false;
        }
    }

    void IntegratedServer::DoSweepAttack(Server::PlayerEntityView& attacker,
                                         Game::LivingEntity& target,
                                         float strengthScale) {
        // The sweep asks "what else is standing next to what I hit", which is
        // a query against the attacker's own level's entity set.
        ServerPlayer* player = attacker.GetPlayer();
        if (!player) return;
        ServerLevel* level = GetLevel(Game::DimensionFromRaw(player->getDimensionId()));
        ServerLevelBridge* mobLevel = level ? level->MobLevel() : nullptr;
        if (!mobLevel) return;

        // MC Player.doSweepAttack: everything living inside the TARGET's box
        // inflated by (1.0, 0.25, 1.0), except the attacker and the target
        // itself, within 3 blocks of the attacker, takes
        // `1.0 * attackStrengthScale` and a 0.4 knockback along the attacker's
        // facing. The 1.0 is `1 + SWEEPING_DAMAGE_RATIO * baseDamage` with the
        // ratio at its unenchanted 0 — the sweep does NOT scale with the
        // weapon, which is why it reads as a nudge to the crowd rather than a
        // second full hit.
        Game::AABB box = target.GetAABB();
        box.min -= glm::vec3(1.0f, 0.25f, 1.0f);
        box.max += glm::vec3(1.0f, 0.25f, 1.0f);

        std::vector<Game::Entity*> nearby;
        mobLevel->GetEntitiesInBox(box, &attacker, nearby);

        const float yaw = attacker.yRot * Game::Mth::kDegToRad;
        const float sweepDamage = 1.0f * strengthScale;

        for (Game::Entity* entity : nearby) {
            if (entity == &target) continue;
            auto* living = dynamic_cast<Game::LivingEntity*>(entity);
            if (!living || !living->IsAlive() || !living->IsAttackable()) continue;
            if (attacker.DistanceToSqr(*living) >= 9.0) continue;

            if (living->Hurt(Game::MobDamageSource::PlayerAttack, sweepDamage, &attacker)) {
                // Same negated-facing convention as the sprint knockback above.
                living->Knockback(0.4, std::sin(yaw), -std::cos(yaw));
            }
        }
    }

    Server::PlayerEntityView* IntegratedServer::GetPlayerEntityView(uint32_t connectionId) {
        // Each level's bridge keeps its own view objects, so a player in the
        // Nether has their live view in the Nether's bridge — asking the
        // overworld's would hand back a stale mirror (or nothing).
        //
        // The dimension is read off the ServerPlayer and NOT via a session
        // lookup, which would deadlock: PlayerSessionManager::
        // BroadcastPlayerPositions calls this from inside its own
        // (non-recursive) m_sessionMutex, so asking that manager for a session
        // here would block the server thread on a lock it already holds.
        //
        // Connection ids and player ids are the same number (OnPlayerJoined
        // uses the connection id as the player id), and both containers below
        // are only ever written on the server thread.
        const ServerPlayer* player = nullptr;
        if (connectionId == 1 && m_serverPlayer) {
            player = m_serverPlayer.get();
        } else {
            auto it = m_remotePlayers.find(connectionId);
            if (it != m_remotePlayers.end()) player = it->second.get();
        }
        if (!player) return nullptr;

        ServerLevel* level = GetLevel(Game::DimensionFromRaw(player->getDimensionId()));
        ServerLevelBridge* mobLevel = level ? level->MobLevel() : nullptr;
        return mobLevel ? mobLevel->GetPlayerView(connectionId) : nullptr;
    }

    void IntegratedServer::BroadcastAttackEffects(Game::DimensionId dimension,
                                                  const Game::LivingEntity& target,
                                                  bool crit) {
        if (!m_networkServer) return;

        // MC broadcasts entity event 2 for a generic hurt, which is what makes
        // a MOB flash red for onlookers. Mobs already flash from their synched
        // hurtTime, so the only new signal here is the crit particle burst
        // (event 4 in this port's numbering, unused by any mob).
        if (!crit) return;

        Network::EntityEventS2CPacket p;
        p.entityId = target.GetId();
        p.event = kEntityEventCrit;
        const auto data = Network::Serialization::Serialize(p);

        const Game::Math::ChunkPos cp{
            static_cast<int32_t>(std::floor(target.position.x / 16.0)),
            static_cast<int32_t>(std::floor(target.position.z / 16.0))
        };
        SendToChunkWatchersAt(dimension, cp, Network::PacketId::EntityEventS2C, data);
    }

    void IntegratedServer::BroadcastItemEntityUpdates(ServerLevel& level, int64_t serverTick) {
        if (!level.Items() || !m_sessionManager) return;

        std::vector<int32_t> fullRefresh;
        std::vector<int32_t> moveOnly;
        level.Items()->CollectSyncSets(serverTick, fullRefresh, moveOnly);

        for (int32_t id : fullRefresh) {
            BroadcastItemEntitySpawn(level, id);
        }

        if (moveOnly.empty()) return;

        // Compact updates are bucketed by chunk so each client gets ONE packet
        // covering everything it can see, rather than one per entity.
        const auto& all = level.Items()->All();
        std::unordered_map<Game::Math::ChunkPos, Network::ItemEntityMoveS2CPacket,
                           Game::Math::ChunkPosHash> byChunk;

        for (int32_t id : moveOnly) {
            auto it = all.find(id);
            if (it == all.end()) continue;
            const Game::ItemEntity& e = it->second;

            Network::ItemEntityMoveS2CPacket::Entry entry;
            entry.entityId = e.id;
            entry.position = e.pos;
            entry.velocity = glm::vec3(e.vel);
            entry.count    = e.stack.count;

            const Game::Math::ChunkPos cp{
                static_cast<int32_t>(std::floor(e.pos.x / 16.0)),
                static_cast<int32_t>(std::floor(e.pos.z / 16.0))
            };
            byChunk[cp].entries.push_back(entry);
        }

        for (const auto& [chunk, packet] : byChunk) {
            const auto data = Network::Serialization::Serialize(packet);
            SendToChunkWatchersAt(level.Dimension(), chunk,
                                  Network::PacketId::ItemEntityMoveS2C, data);
        }
    }

    void IntegratedServer::BroadcastItemEntityPickups(Game::DimensionId dimension,
            const std::vector<ItemPickupEvent>& pickups) {
        if (pickups.empty() || !m_sessionManager) return;

        // Scoped to the item's dimension like every other positional packet.
        // This used to be an unscoped broadcast ("a client that never knew
        // the entity ignores it"), which stopped being true with several
        // levels per client: an unscoped packet lands in whichever level the
        // client's stream was last pointed at, and while a portal view is
        // streaming another dimension that is often the wrong one — the take
        // then retires nothing and the item stays on the floor.
        for (const auto& p : pickups) {
            Network::TakeItemEntityS2CPacket packet;
            packet.itemEntityId = p.itemEntityId;
            packet.playerId     = p.playerId;
            packet.amount       = p.amount;

            const auto data = Network::Serialization::Serialize(packet);
            for (const auto& session : m_sessionManager->GetAllSessions()) {
                if (!session || !session->GetConnection()) continue;
                if (!session->LoadsDimension(dimension)) continue;
                session->GetConnection()->SendPacketIn(dimension,
                    static_cast<uint8_t>(Network::PacketId::TakeItemEntityS2C), data);
            }
        }
    }

    void IntegratedServer::BroadcastItemEntityRemovals(Game::DimensionId dimension,
                                                       const std::vector<int32_t>& ids) {
        if (ids.empty() || !m_sessionManager) return;

        // Scoped to the dimension, where it used to be a flat broadcast.
        //
        // The old comment was right for one world — "a client that never knew
        // the entity just doesn't find the id in its map" — and became wrong
        // the moment there were three. Item, orb and mob ids are allocated per
        // LEVEL from the same per-type base, so a Nether item and an Overworld
        // item genuinely share an id, and an unscoped removal would retire the
        // wrong one on the wrong client.
        //
        // Still unscoped WITHIN the dimension: tracking who was told about
        // which item costs more than it saves, and a client in this dimension
        // that never saw the id simply ignores it — which is what the original
        // note was actually about.
        Network::RemoveEntitiesS2CPacket packet(ids);
        const auto data = Network::Serialization::Serialize(packet);

        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (!session || !session->GetConnection()) continue;
            // Every session that holds chunks of this dimension — with
            // immersive portals that includes a player standing in another
            // one, looking in through a surface.
            if (!session->LoadsDimension(dimension)) continue;
            session->GetConnection()->SendPacketIn(dimension,
                static_cast<uint8_t>(Network::PacketId::EntityDestroy), data);
        }
    }

    // ========================================================================
    // EXPERIENCE ORB BROADCAST
    // ========================================================================

    void IntegratedServer::BroadcastXpOrbSpawn(ServerLevel& level, int32_t id) {
        if (!level.Orbs() || !m_sessionManager) return;

        const auto& all = level.Orbs()->All();
        auto it = all.find(id);
        if (it == all.end()) return;
        const Game::ExperienceOrb& orb = it->second;

        Network::XpOrbSpawnS2CPacket packet;
        packet.entityId = orb.id;
        packet.position = orb.pos;
        packet.velocity = glm::vec3(orb.vel);
        packet.value    = orb.value;

        const auto data = Network::Serialization::Serialize(packet);
        SendToChunkWatchers(level.Dimension(), orb.pos,
                            Network::PacketId::XpOrbSpawnS2C, data);
    }

    void IntegratedServer::BroadcastXpOrbUpdates(ServerLevel& level, int64_t serverTick) {
        if (!level.Orbs() || !m_sessionManager) return;

        std::vector<int32_t> fullRefresh;
        std::vector<int32_t> moveOnly;
        level.Orbs()->CollectSyncSets(serverTick, fullRefresh, moveOnly);

        for (int32_t id : fullRefresh) {
            BroadcastXpOrbSpawn(level, id);
        }

        if (moveOnly.empty()) return;

        // Bucketed by chunk like the item moves — one packet per chunk per
        // client instead of one per orb.
        const auto& all = level.Orbs()->All();
        std::unordered_map<Game::Math::ChunkPos, Network::XpOrbMoveS2CPacket,
                           Game::Math::ChunkPosHash> byChunk;

        for (int32_t id : moveOnly) {
            auto it = all.find(id);
            if (it == all.end()) continue;
            const Game::ExperienceOrb& orb = it->second;

            Network::XpOrbMoveS2CPacket::Entry entry;
            entry.entityId = orb.id;
            entry.position = orb.pos;
            entry.velocity = glm::vec3(orb.vel);

            const Game::Math::ChunkPos cp{
                static_cast<int32_t>(std::floor(orb.pos.x / 16.0)),
                static_cast<int32_t>(std::floor(orb.pos.z / 16.0))
            };
            byChunk[cp].entries.push_back(entry);
        }

        for (const auto& [chunk, packet] : byChunk) {
            const auto data = Network::Serialization::Serialize(packet);
            SendToChunkWatchersAt(level.Dimension(), chunk,
                                  Network::PacketId::XpOrbMoveS2C, data);
        }
    }

    void IntegratedServer::BroadcastXpOrbPickups(Game::DimensionId dimension,
            const std::vector<XpOrbPickupEvent>& pickups) {
        if (pickups.empty() || !m_sessionManager) return;

        // MC broadcasts ClientboundTakeItemEntityPacket for orbs too; the
        // client routes on the id range. Dimension-scoped — see
        // BroadcastItemEntityPickups.
        for (const auto& p : pickups) {
            Network::TakeItemEntityS2CPacket packet;
            packet.itemEntityId = p.orbId;
            packet.playerId     = p.playerId;
            packet.amount       = 1;

            const auto data = Network::Serialization::Serialize(packet);
            for (const auto& session : m_sessionManager->GetAllSessions()) {
                if (!session || !session->GetConnection()) continue;
                if (!session->LoadsDimension(dimension)) continue;
                session->GetConnection()->SendPacketIn(dimension,
                    static_cast<uint8_t>(Network::PacketId::TakeItemEntityS2C), data);
            }
        }
    }

    void IntegratedServer::SendToChunkWatchers(Game::DimensionId dimension,
                                              const glm::dvec3& pos,
                                              Network::PacketId packetId,
                                              const std::vector<uint8_t>& data) {
        const Game::Math::ChunkPos cp{
            static_cast<int32_t>(std::floor(pos.x / 16.0)),
            static_cast<int32_t>(std::floor(pos.z / 16.0))
        };
        SendToChunkWatchersAt(dimension, cp, packetId, data);
    }

    void IntegratedServer::SendToChunkWatchersAt(Game::DimensionId dimension,
                                                Game::Math::ChunkPos chunk,
                                                Network::PacketId packetId,
                                                const std::vector<uint8_t>& data) {
        if (!m_sessionManager) return;

        m_sessionManager->ForEachSessionWatching(dimension, chunk,
                                                 [&](PlayerSession& session) {
            if (auto* conn = session.GetConnection()) {
                conn->SendPacketIn(dimension, static_cast<uint8_t>(packetId), data);
            }
        });
    }

    void IntegratedServer::OnChunkSentToClient(PlayerSession& session, Game::DimensionId dimension,
                                               Game::Math::ChunkPos chunk) {
#if ENABLE_IMMERSIVE_PORTALS
        if (!m_immersivePortals) return;
        auto* conn = session.GetConnection();
        if (!conn) return;
        // The dimension's global surfaces ride the first chunk the client
        // gets of it: by then the client holds a level for the dimension.
        if (session.MarkGlobalPortalsSynced(dimension)) {
            m_immersivePortals->SyncGlobalsToClient(*conn, dimension);
            SendAoRegions(*conn, dimension);
        }
        m_immersivePortals->SyncChunkToClient(*conn, dimension, chunk);
#else
        (void)session; (void)dimension; (void)chunk;
#endif
    }

    void IntegratedServer::OnFrameBlockRemoved(Game::DimensionId dimension, const glm::ivec3& pos,
                                               Game::BlockID removed) {
#if ENABLE_IMMERSIVE_PORTALS
        if (m_netherPortalGeneration) m_netherPortalGeneration->OnFrameBlockRemoved(dimension, pos, removed);
#else
        (void)dimension; (void)pos; (void)removed;
#endif
    }

#if ENABLE_IMMERSIVE_PORTALS
    void IntegratedServer::SetRedstoneChunks(bool on) {
        m_config.redstoneChunks = on;
        Log::Info("[ChunkKeeper] redstone chunks %s", on ? "on" : "off");
        // The keepers pick the flag up in ServiceKeptChunks.
    }

    void IntegratedServer::ServiceKeptChunks() {
        PROFILE_ZONE;
        ForEachLevel([&](ServerLevel& level) {
            ChunkKeeper* keeper = level.Keeper();
            Game::World* world = level.World();
            if (!keeper || !world) return;
            keeper->SetRedstoneEnabled(m_config.redstoneChunks);
            const Game::DimensionId dim = level.Dimension();
            keeper->Service(m_currentServerTick,
                [&](Game::Math::ChunkPos pos) {
                    return world->IsChunkLoaded(pos.x, pos.z) || level.pendingChunkLoads.count(pos) != 0;
                },
                [&](Game::Math::ChunkPos pos) { RequestChunkLoad(dim, pos, 0); });
            // Redstone warm-up (ServerLevel::redstoneWarmup): freeze redstone
            // while kept chunks are still arriving; when the last one is in,
            // settle every border that arrived meanwhile, then let it run.
            if (!Game::RedstonePlus::Enabled()) {
                if (level.redstoneWarmup) { level.redstoneWarmup = false; level.redstoneSettleLater.clear(); }
                world->SetRedstoneFrozen(false);
                return;
            }
            if (!level.redstoneWarmup && !level.redstoneWarmupDone && keeper->KeptCount() > 0) {
                level.redstoneWarmup = true;
                world->SetRedstoneFrozen(true);
                Log::Info("[RedstonePlus] warm-up: %zu kept chunk(s) to load before redstone runs", keeper->KeptCount());
            }
            if (level.redstoneWarmup && m_currentServerTick % 5 == 0 && !keeper->ScanRunning()) {
                bool allResident = true;
                keeper->ForEachKept([&](Game::Math::ChunkPos pos) {
                    if (allResident && !world->IsChunkLoaded(pos.x, pos.z)) allResident = false;
                });
                if (allResident) {
                    for (const Game::Math::ChunkPos& pos : level.redstoneSettleLater) SettleRedstoneBorders(level, pos);
                    const size_t settled = level.redstoneSettleLater.size();
                    level.redstoneSettleLater.clear();
                    level.redstoneWarmup = false;
                    level.redstoneWarmupDone = true;
                    world->SetRedstoneFrozen(false);
                    Log::Info("[RedstonePlus] warm-up done: %zu kept chunk(s) resident, %zu border settle(s) run",
                              keeper->KeptCount(), settled);
                }
            }
        });
    }

    void IntegratedServer::SetRedstonePlus(bool on) {
        m_config.redstonePlus = on;
        Game::RedstonePlus::SetEnabled(on);
        BroadcastWorldRules();
        Log::Info("[RedstonePlus] %s — networks re-evaluate as they next update", on ? "on" : "off");
    }

    void IntegratedServer::SetModDimensionEnabled(Game::DimensionId dimension, bool on) {
        if (dimension == Game::DimensionId::TwilightForest) m_config.twilightForestEnabled = on;
        else if (dimension == Game::DimensionId::Aether)    m_config.aetherEnabled = on;
        else return;
        Game::ModDimensions::SetEnabled(dimension, on);
        Log::Info("[ModDimensions] %s %s — its portals %s", std::string(Game::DimensionName(dimension)).c_str(),
                  on ? "on" : "off", on ? "work again" : "no longer send anyone in");
    }

    void IntegratedServer::SetSharedVitals(bool on) {
        m_config.sharedVitals = on;
        if (m_sessionManager) m_sessionManager->ResetSharedVitals();
        Log::Info("[SharedVitals] %s — every player now %s", on ? "on" : "off",
                  on ? "shares one health and hunger" : "keeps their own");
    }

    void IntegratedServer::SetVeinMineMaxBlocks(int count) {
        m_config.veinMineMaxBlocks = std::clamp(count, 0, kMaxVeinMineMaxBlocks);
        Log::Info("[VeinMine] limit is now %d extra block(s)", m_config.veinMineMaxBlocks);
    }

    void IntegratedServer::SetImmersivePortals(bool on) {
        const bool was = m_config.immersivePortals;
        m_config.immersivePortals = on;
        Game::Portals::SetImmersiveNetherPortals(on);
        BroadcastWorldRules();
        Log::Info("[ImmersivePortals] nether portals are now %s", on ? "immersive" : "vanilla");
        if (was && !on && m_immersivePortals) {
            // Turning the rule off used to flip only the flag: the portals
            // already standing stayed see-through and walk-through, so the
            // rule looked dead. Every portal the rule governs goes now
            // (removal is broadcast): nether portals, command portals and
            // mirrors are PARKED in the world's data folder so `true` brings
            // them back; gun surfaces are rebuilt from the gun pairs then.
            // Each nether frame gets purple portal blocks FIRST, on both ends
            // of every pair, so the frames keep working and vanilla's
            // nearest-portal link (PortalForcer, the 8:1 scaling) lands on
            // the same counterpart the immersive pair had. Hush and Aether
            // portals are not the rule's: they are always vanilla blocks
            // (Portals::FamilyIsImmersive) and nothing here touches them.
            FillNetherFramesWithPortalBlocks();
            m_immersivePortals->StashAll();
            if (m_netherPortalGeneration) m_netherPortalGeneration->ClearPending(Game::PortalFamilyId::Nether);
        } else if (!was && on && m_immersivePortals) {
            m_immersivePortals->RestoreStashed();
            // The surfaces are back; the purple blocks inside their frames
            // would be inert AND drawn, so they go.
            ClearPortalBlocksInImmersiveFrames();
#if ENABLE_PORTAL_GUN
            Game::Portal::ServerRegistry().RebuildImmersive();
#endif
        }
        // level.dat carries the flag (meta.immersivePortals).
        if (was != on) WriteLevelDat();
    }

    void IntegratedServer::BroadcastWorldRules() {
        if (!m_sessionManager) return;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (session && session->GetConnection()) session->GetConnection()->SendWorldRules();
        }
    }

    // The frames of every immersive frame portal the rule governs (the
    // nether's; hush and aether portals are always vanilla blocks), once each (a bi-way bi-faced cluster is four portal records
    // over two frames).
    namespace {
        struct FrameKey {
            Game::DimensionId dim; glm::ivec3 minCell;
            bool operator==(const FrameKey& o) const { return dim == o.dim && minCell == o.minCell; }
        };
    }

    void IntegratedServer::FillNetherFramesWithPortalBlocks() {
        if (!m_immersivePortals) return;
        std::vector<FrameKey> seen;
        size_t filled = 0;
        m_immersivePortals->ForEach([&](const Game::Immersive::Portal& p) {
            const Game::PortalFamily* fam = Game::FamilyOfKind(p.kind);
            if (!fam || !Game::Portals::FamilyFollowsImmersiveRule(fam->id)) return;
            const auto frame = Game::Immersive::FrameFromPortal(p);
            if (!frame || frame->area.empty()) return;
            const FrameKey key{p.dimension, frame->minCell};
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
            seen.push_back(key);
            ServerLevel* level = GetLevel(p.dimension);
            Game::World* world = level ? level->World() : nullptr;
            if (!world) return;
            // Vanilla's own frame walk from an interior cell (the flint-and-
            // steel path), so the result is exactly a lit vanilla portal of
            // the family.
            const Game::Axis preferred = frame->axis == Game::Axis::X ? Game::Axis::X : Game::Axis::Z;
            // Already lit (blocks left from an earlier off period): nothing
            // to do, and not a shape complaint.
            bool alreadyLit = false;
            for (const glm::ivec3& c : frame->area) {
                if (world->GetBlock(c.x, c.y, c.z) == fam->portalBlock) { alreadyLit = true; break; }
            }
            if (alreadyLit) { ++filled; return; }
            if (auto shape = Game::PortalShape::FindEmptyPortalShape(*world, frame->area.front(), preferred, *fam)) {
                shape->CreatePortalBlocks(*world);
                ++filled;
            } else {
                Log::Warning("[ImmersivePortals] %s frame at (%d,%d,%d) is not a vanilla-shaped frame; left dark",
                             fam->tag, frame->minCell.x, frame->minCell.y, frame->minCell.z);
            }
        });
        Log::Info("[ImmersivePortals] lit %zu frame(s) as vanilla portals", filled);
    }

    void IntegratedServer::ClearPortalBlocksInImmersiveFrames() {
        if (!m_immersivePortals) return;
        std::vector<FrameKey> seen;
        size_t cleared = 0;
        m_immersivePortals->ForEach([&](const Game::Immersive::Portal& p) {
            const Game::PortalFamily* fam = Game::FamilyOfKind(p.kind);
            if (!fam || !Game::Portals::FamilyFollowsImmersiveRule(fam->id)) return;
            const auto frame = Game::Immersive::FrameFromPortal(p);
            if (!frame) return;
            const FrameKey key{p.dimension, frame->minCell};
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
            seen.push_back(key);
            ServerLevel* level = GetLevel(p.dimension);
            Game::World* world = level ? level->World() : nullptr;
            if (!world) return;
            for (const glm::ivec3& c : frame->area) {
                if (world->GetBlock(c.x, c.y, c.z) != fam->portalBlock) continue;
                world->SetBlock(c.x, c.y, c.z, Game::BlockID::Air, Game::World::UpdateFlags::All);
                ++cleared;
            }
        });
        if (cleared) Log::Info("[ImmersivePortals] cleared %zu vanilla portal block(s) from restored frames", cleared);
    }

    void IntegratedServer::ConvertVanillaFamilyPortals() {
        if (!m_immersivePortals) return;
        // Every record of a family that is vanilla now (Portals::
        // FamilyIsImmersive: the Hush and the Aether, always). A pair is one
        // cluster over two frames and goes as one, so BOTH frames are lit:
        // a far frame left dark would make vanilla's nearest-portal search
        // (PortalForcer) build a second portal beside it on the first
        // crossing.
        struct Target {
            Game::DimensionId dimension;
            Game::Immersive::FrameShape frame;
            const Game::PortalFamily* family;
        };
        std::vector<Target> targets;
        std::vector<Game::Immersive::PortalId> records;
        std::vector<FrameKey> seen;
        m_immersivePortals->ForEach([&](const Game::Immersive::Portal& p) {
            const Game::PortalFamily* fam = Game::FamilyOfKind(p.kind);
            if (!fam || Game::Portals::FamilyIsImmersive(fam->id)) return;
            records.push_back(p.id);
            const auto frame = Game::Immersive::FrameFromPortal(p);
            if (!frame || frame->area.empty()) return;
            const FrameKey key{p.dimension, frame->minCell};
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
            seen.push_back(key);
            targets.push_back({p.dimension, *frame, fam});
        });
        if (records.empty()) return;

        size_t lit = 0;
        for (const Target& t : targets) {
            ServerLevel* level = GetOrCreateLevel(t.dimension);
            Game::World* world = level ? level->World() : nullptr;
            Game::ChunkProvider* provider = world ? world->GetChunkProvider() : nullptr;
            if (!provider) continue;
            // Resident first — an unloaded chunk reads as air, and the shape
            // walk would run straight out of the frame. Blocking, as the
            // spawn search's probes are; this thread is the one that pumps
            // generation.
            for (int cx = (t.frame.minCell.x - 1) >> 4; cx <= (t.frame.maxCell.x + 1) >> 4; ++cx) {
                for (int cz = (t.frame.minCell.z - 1) >> 4; cz <= (t.frame.maxCell.z + 1) >> 4; ++cz) {
                    provider->GetChunk(Game::Math::ChunkPos(cx, cz));
                }
            }
            bool alreadyLit = false;
            for (const glm::ivec3& c : t.frame.area) {
                if (world->GetBlock(c.x, c.y, c.z) == t.family->portalBlock) { alreadyLit = true; break; }
            }
            if (alreadyLit) { ++lit; continue; }
            // Vanilla's own frame walk from an interior cell (the ignition
            // path), so the result is exactly a lit vanilla portal.
            const Game::Axis preferred = t.frame.axis == Game::Axis::X ? Game::Axis::X : Game::Axis::Z;
            if (auto shape = Game::PortalShape::FindEmptyPortalShape(*world, t.frame.area.front(), preferred,
                                                                     *t.family)) {
                shape->CreatePortalBlocks(*world);
                ++lit;
            } else {
                Log::Warning("[Portals] %s frame at (%d,%d,%d) in %s is not a vanilla-shaped frame; left dark",
                             t.family->tag, t.frame.minCell.x, t.frame.minCell.y, t.frame.minCell.z,
                             std::string(Game::DimensionName(t.dimension)).c_str());
            }
        }
        size_t removed = 0;
        for (Game::Immersive::PortalId id : records) {
            if (m_immersivePortals->Get(id)) removed += m_immersivePortals->RemoveCluster(id);
        }
        Log::Info("[Portals] converted %zu hush/aether frame(s) to vanilla portals (%zu immersive record(s) removed)",
                  lit, removed);
    }

    void IntegratedServer::SetPortalGunAllowed(bool on) {
        const bool was = Game::Portals::PortalGunAllowed();
        Game::Portals::SetPortalGunAllowed(on);
        BroadcastWorldRules();
#if ENABLE_PORTAL_GUN
        if (was && !on) {
            // Close every pair (fizzle burst + immersive surface removal are
            // broadcast by ClearPair).
            std::vector<uint64_t> guns;
            for (const auto& [gunId, pair] : Game::Portal::ServerRegistry().All()) guns.push_back(gunId);
            for (uint64_t gunId : guns) Game::Portal::ServerRegistry().ClearPair(gunId);
            Log::Info("[PortalGun] disabled by gamerule — closed %zu portal pairs", guns.size());
        }
#endif
        if (was != on) PersistPortalGunAllowed();
    }

    void IntegratedServer::PersistPortalGunAllowed() {
        if (m_config.savePath.empty() || m_config.readOnlyWorld) return;
        std::string reason;
        auto root = Game::Anvil::SaveRoot::Open(m_config.savePath, reason);
        if (!root) return;
        const std::string dir = root->Root().string();
        Game::Anvil::WorldSidecar sidecar = Game::Anvil::ReadWorldSidecar(dir);
        if (sidecar.portalGun == Game::Portals::PortalGunAllowed()) return;
        sidecar.portalGun = Game::Portals::PortalGunAllowed();
        if (!Game::Anvil::WriteWorldSidecar(dir, sidecar)) {
            Log::Warning("[PortalGun] Could not save the portal_gun rule to %s", dir.c_str());
        }
    }

    bool IntegratedServer::OnImmersiveFrameLit(Game::ILevelWrite& level, const glm::ivec3& seedPos,
                                               Game::PortalFamilyId family) {
        IntegratedServer* server = g_integratedServer.get();
        if (!server || !server->m_netherPortalGeneration) return false;
        const Game::DimensionId dimension = level.GetDimension();
        const auto shape = Game::Immersive::FrameShape::Find(level, seedPos, family);
        if (!shape) return false;
        return server->m_netherPortalGeneration->OnFrameLit(dimension, *shape);
    }

    void IntegratedServer::OnClientPortalTeleport(PlayerSession& session,
                                                  const Network::PortalTeleportC2SPacket& packet) {
        ASSERT_SERVER_THREAD();
        ServerPlayer* player = session.GetPlayer();
        if (!player || !m_immersivePortals) return;

        // The mod's bounds: the client is trusted about WHERE it crossed,
        // within reason.
        constexpr double kMaxPositionError  = 16.0;
        constexpr double kMaxPortalDistance = 20.0;

        const Game::DimensionId here = Game::DimensionFromRaw(player->getDimensionId());
        const glm::dvec3 eyeBefore(packet.eyeX, packet.eyeY, packet.eyeZ);
        const glm::dvec3 serverFeet = player->getPosition();

        auto reject = [&](const char* why) {
            Log::Warning("[ImmersivePortals] Rejected portal crossing of '%s' through #%u: %s",
                         player->getName().c_str(), packet.portalId, why);
            session.SendDimensionResync();
            if (auto* conn = session.GetConnection()) {
                conn->Teleport(serverFeet.x, serverFeet.y, serverFeet.z,
                               player->getYaw(), player->getPitch());
            }
        };

        const Game::Immersive::Portal* portal = m_immersivePortals->Get(packet.portalId);
        if (!portal) { reject("unknown portal"); return; }
        if (!portal->Has(Game::Immersive::PortalFlag::Teleportable)) { reject("not teleportable"); return; }
        if (portal->specificPlayerId != 0 && portal->specificPlayerId != player->getPlayerId()) {
            reject("not this player's portal"); return;
        }
        if (portal->dimension != here || Game::DimensionFromRaw(packet.dimensionBefore) != here) {
            reject("dimension mismatch"); return;
        }
        const double eyeHeight = std::clamp(eyeBefore.y - serverFeet.y, 0.5, 2.0);
        if (glm::length(eyeBefore - (serverFeet + glm::dvec3(0.0, eyeHeight, 0.0))) > kMaxPositionError) {
            reject("too far from the server's position"); return;
        }
        {
            glm::dvec3 mn, mx;
            portal->BoundingBox(mn, mx, 0.0);
            if (glm::length(glm::clamp(eyeBefore, mn, mx) - eyeBefore) > kMaxPortalDistance) {
                reject("too far from the portal"); return;
            }
        }

        if (!TeleportPlayerThroughPortal(session, *portal, eyeBefore, packet.yaw, packet.pitch,
                                         /*clientPredicted=*/true)) {
            reject("destination dimension unavailable");
        }
    }

    bool IntegratedServer::TeleportPlayerThroughPortal(PlayerSession& session,
                                                       const Game::Immersive::Portal& portal,
                                                       const glm::dvec3& eyeBefore,
                                                       float yaw, float pitch, bool clientPredicted) {
        ASSERT_SERVER_THREAD();
        ServerPlayer* player = session.GetPlayer();
        if (!player) return false;
        const Game::DimensionId here = Game::DimensionFromRaw(player->getDimensionId());
        const glm::dvec3 serverFeet = player->getPosition();
        const double eyeHeight = std::clamp(eyeBefore.y - serverFeet.y, 0.05, 40.0);
        // The body scales with the portal (the client does the same): the
        // eye offset maps through the scale so the feet land on the far
        // ground, and the player's size is multiplied for everything that
        // reads it — reach, eye height, the placement box.
        const double portalScale = portal.IsMirror() ? 1.0 : portal.scale;
        const glm::dvec3 newEye  = portal.TransformPoint(eyeBefore);
        const glm::dvec3 newFeet = newEye - glm::dvec3(0.0, eyeHeight * portalScale, 0.0);
        const Game::DimensionId dest = portal.IsMirror() ? here : portal.destDimension;
        // MC ServerLevel.isAllowedToEnterPortal: the Nether is closed while
        // allow_entering_nether_using_portals is off — the crossing is
        // refused before anything about the player changes.
        if (dest == Game::DimensionId::Nether && dest != here &&
            !Game::Rules::GetBool(Game::Rules::Id::AllowEnteringNetherUsingPortals)) {
            return false;
        }
        player->setScale(static_cast<float>(player->getScale() * portalScale));

        // The mobs hunting this player follow them to the portal (and
        // through it, see EntityPortalTravel). Before the dimension flips:
        // the player's view in the level being left is what they target.
        if (m_entityTravel) {
            if (ServerLevel* from = GetLevel(here)) {
                m_entityTravel->OnPlayerCrossed(*from, portal, session.GetConnectionId(),
                                                m_currentServerTick);
            }
        }

        if (dest != here) {
            ServerLevel* from = GetLevel(here);
            ServerLevel* to   = GetOrCreateLevel(dest);
            if (!to) return false;
            // Player tickets are per level; drop them where the player was
            // BEFORE the dimension flips, or the removal aims at the wrong
            // manager (see PortalTravel::MovePlayer).
            if (from && from->Tickets()) from->Tickets()->RemoveAllPlayerTickets(player->getPlayerId());
            session.ChangeDimension(Game::DimensionToRaw(dest), glm::vec3(newFeet), /*keepPrevious=*/true);
        } else {
            player->teleport(newFeet);
            session.ResyncChunkPosition();
        }
        player->setRotation(yaw, pitch);
        // A crossing the client did not predict: it is still standing where
        // it was. ChangeDimension only switches its level (a seamless
        // crossing carries no position — the predicting client already
        // moved), so the position goes separately, after it, in every case.
        // Without this the client kept its old coordinates in the new
        // dimension — the far side of the world, at sea, on one report.
        if (!clientPredicted) {
            if (auto* conn = session.GetConnection()) {
                conn->Teleport(newFeet.x, newFeet.y, newFeet.z, yaw, pitch);
                // The size changed on the server alone: the abilities packet
                // carries it to the client.
                conn->SendPlayerAbilities(*player);
            }
        }
        // The arrival point may sit inside a vanilla portal block (the
        // frame's interior); its own transition must not fire on top.
        player->portalState().SetCooldown(Game::Portals::kPlayerPortalCooldown);
        // The server's own crossing watch starts over on the far side.
        if (m_entityTravel) {
            m_entityTravel->OnPlayerTeleported(session.GetConnectionId(), dest, newEye,
                                               m_currentServerTick);
        }
        // A gun pair flashes when something goes through it.
        Game::Portal::ServerRegistry().OnImmersiveCrossing(portal.tag);
        // The Hush's recall chime remembers the last hush gate crossed: the
        // far-side arrival (server/items/HushItems), as PortalTravel records
        // it for a vanilla-mode crossing.
        if (portal.kind == Game::Immersive::PortalKind::HushPortal && dest != here) {
            HushItems::RecordGateCrossing(*player, dest, newFeet, yaw);
        }

        Log::Info("[ImmersivePortals] '%s' crossed #%u into %s at (%.1f, %.1f, %.1f)%s",
                  player->getName().c_str(), portal.id,
                  std::string(Game::DimensionName(dest)).c_str(), newFeet.x, newFeet.y, newFeet.z,
                  clientPredicted ? "" : " (server-detected)");
        return true;
    }
#endif

    std::vector<ChunkLoader> IntegratedServer::ComputeChunkLoaders(const PlayerSession& session) const {
        std::vector<ChunkLoader> loaders;
        const Game::DimensionId here = Game::DimensionFromRaw(session.GetDimensionId());

        // 1. The player's own view — MC's one and only loader.
        {
            ChunkLoader own;
            own.dimension = here;
            own.view      = ChunkTrackingView::Of(session.GetAnchorChunk(), session.GetViewDistance());
            own.source    = ChunkLoader::Source::Player;
            loaders.push_back(own);
        }

        // 1b. The simulation ring beyond the view. Block ticking reaches one
        //     chunk further than entity ticking (ChunkLevel::BLOCK_TICKING),
        //     so the loader is one wider than the simulation distance; the
        //     chunks it adds are loaded and ticked but never sent. Nothing to
        //     add when the view already covers it (MC's only case).
        if (session.GetSimulationDistance() + 1 > session.GetViewDistance()) {
            ChunkLoader sim;
            sim.dimension = here;
            sim.view      = ChunkTrackingView::Of(session.GetAnchorChunk(), session.GetSimulationDistance() + 1);
            sim.source    = ChunkLoader::Source::Simulation;
            loaders.push_back(sim);
        }

#if ENABLE_IMMERSIVE_PORTALS
        // 2. The far side of every portal near the player, and 3. one level
        //    deep, the far sides of the portals near THOSE far sides — the
        //    Immersive Portals mod's ChunkVisibility.foreachBaseChunkLoaders,
        //    number for number:
        //
        //    • Portals are looked for within 8 chunks; global surfaces
        //      (wrap borders, stack seams) within 256 chunks. Over a
        //      hundred hits keep only the nearest.
        //    • A direct loader (getGeneralDirectPortalLoader) sits at the
        //      portal's destination with the view distance within 15 blocks
        //      of the surface (the mod: within 5, two thirds to 15) and a
        //      third beyond. The
        //      mod caps this at 8 chunks (IPGlobal.indirectLoadingRadiusCap,
        //      16 for scale > 2); here it is UNCAPPED on purpose (user's
        //      call, 2026-09-05): standing at a portal at render distance
        //      32 shows 32 chunks of the far side, not a 128-block disc
        //      with a hard edge in the far fog. A portal that enlarges the
        //      far side (scale > 2) within 5 blocks asks for 1.4 × its far
        //      area's radius instead. A global surface's loader follows the
        //      player's IMAGE through it with the view distance less the
        //      distance to the plane in chunks, at least 2, at most 16.
        //    • Indirect loaders (getGeneralPortalIndirectLoader): portals
        //      within 2 chunks of the player's image get a quarter of the
        //      view distance, capped the same way; a global surface there
        //      gets min(8, a third), around the image's image.
        //
        //    The mod scales these by measured client and server performance
        //    levels; here they are the mod's "good" values, by choice.
        if (m_immersivePortals && session.GetPlayer()) {
            constexpr int kLoadingRadiusCap      = 8;   // IPGlobal.indirectLoadingRadiusCap (indirect + global loaders)
            constexpr int kVisibleRangeChunks    = 8;   // PerformanceLevel.getVisiblePortalRangeChunks(good)
            constexpr int kIndirectRangeChunks   = 2;   // getIndirectVisiblePortalRangeChunks(good)
            const int viewDistance = session.GetViewDistance();   // McHelper.getPlayerLoadDistance
            const uint32_t playerId = session.GetPlayerId();
            const glm::dvec3 playerPos = session.GetPlayer()->getPosition();

            auto usable = [&](const Game::Immersive::Portal& p) {
                if (!p.Has(Game::Immersive::PortalFlag::Visible)) return false;
                if (p.specificPlayerId != 0 && p.specificPlayerId != playerId) return false;
                return true;
            };
            auto distanceTo = [](const Game::Immersive::Portal& p, const glm::dvec3& from) {
                glm::dvec3 mn, mx;
                p.BoundingBox(mn, mx, 0.0);
                const glm::dvec3 c = glm::clamp(from, mn, mx);
                return glm::length(c - from);
            };
            auto portalScale = [](const Game::Immersive::Portal& p) {
                return p.IsMirror() ? 1.0 : p.scale;
            };
            auto farPosOf = [](const Game::Immersive::Portal& p) {
                return p.IsMirror() ? p.origin : p.destination;
            };
            auto chunkOf = [](const glm::dvec3& p) {
                return Game::Math::ChunkPos{ static_cast<int>(std::floor(p.x)) >> 4,
                                             static_cast<int>(std::floor(p.z)) >> 4 };
            };
            // ChunkVisibility.getDirectLoadingDistance, with the mod's middle
            // band (two thirds between 5 and 15 blocks) folded into the
            // near one (user's call, 2026-09-05): the whole render distance
            // out to 15 blocks, a third beyond.
            auto directLoadingDistance = [](int renderDistance, double distanceToPortal) {
                if (distanceToPortal < 15.0) return renderDistance;
                return renderDistance / 3;
            };
            // ChunkVisibility.getCappedLoadingDistance.
            auto cappedLoadingDistance = [&](const Game::Immersive::Portal& p, int target) {
                int cap = kLoadingRadiusCap;
                if (portalScale(p) > 2.0) cap *= 2;   // load more for a scaling portal
                return std::min(target, cap);
            };
            auto add = [&](Game::DimensionId dim, Game::Math::ChunkPos center, int radius,
                           ChunkLoader::Source source) {
                radius = std::max(radius, 1);
                // Same centre, same dimension: keep the larger — a set union
                // does not care, and it keeps the ticket count down.
                for (ChunkLoader& existing : loaders) {
                    if (existing.IsSimulation()) continue;     // never sent: a portal loader must not fold into it
                    if (existing.dimension == dim && existing.view.center == center) {
                        if (radius > existing.view.viewDistance) {
                            existing.view = ChunkTrackingView::Of(center, radius);
                            if (existing.source != ChunkLoader::Source::Player) existing.source = source;
                        }
                        return;
                    }
                }
                ChunkLoader l;
                l.dimension = dim;
                l.view      = ChunkTrackingView::Of(center, radius);
                l.source    = source;
                loaders.push_back(l);
            };
            // ChunkVisibility.getNearbyPortals: ordinary portals within
            // `radiusChunks` of `pos`, global ones within
            // `radiusChunksForGlobals`; more than a hundred keeps the nearest.
            auto nearbyPortals = [&](Game::DimensionId dim, const glm::dvec3& pos,
                                     int radiusChunks, int radiusChunksForGlobals) {
                std::vector<const Game::Immersive::Portal*> result;
                for (const Game::Immersive::Portal* p :
                     m_immersivePortals->CollectNear(dim, pos, static_cast<double>(radiusChunks) * 16.0)) {
                    if (p->Has(Game::Immersive::PortalFlag::Global)) continue;   // below
                    if (usable(*p)) result.push_back(p);
                }
                m_immersivePortals->ForEachInDimension(dim, [&](const Game::Immersive::Portal& p) {
                    if (!p.Has(Game::Immersive::PortalFlag::Global) || !usable(p)) return;
                    if (distanceTo(p, pos) < static_cast<double>(radiusChunksForGlobals) * 16.0) result.push_back(&p);
                });
                if (result.size() > 100) {
                    Log::Warning("[ImmersivePortals] too many portals near (%.0f, %.0f, %.0f) in %s: %zu",
                                 pos.x, pos.y, pos.z, std::string(Game::DimensionName(dim)).c_str(), result.size());
                    const Game::Immersive::Portal* nearest = nullptr;
                    double best = 1e300;
                    for (const Game::Immersive::Portal* p : result) {
                        const double d = distanceTo(*p, pos);
                        if (d < best) { best = d; nearest = p; }
                    }
                    result.clear();
                    if (nearest) result.push_back(nearest);
                }
                return result;
            };

            for (const Game::Immersive::Portal* portal : nearbyPortals(here, playerPos, kVisibleRangeChunks, 256)) {
                const glm::dvec3 transformedPlayerPos = portal->TransformPoint(playerPos);

                // getGeneralDirectPortalLoader.
                if (portal->Has(Game::Immersive::PortalFlag::Global)) {
                    const double distance = distanceTo(*portal, playerPos);
                    // Load a little more to make the dimension stack more
                    // complete: at least 2, at most twice the global cap.
                    const int radius = std::min(kLoadingRadiusCap * 2,
                                                std::max(2, viewDistance - static_cast<int>(std::floor(distance / 16.0))));
                    add(portal->destDimension, chunkOf(transformedPlayerPos), radius, ChunkLoader::Source::Portal);
                } else {
                    int loadDistance = viewDistance;
                    const double distance = distanceTo(*portal, playerPos);
                    if (portalScale(*portal) > 2.0 && distance < 5.0) {
                        // Portal.getDestAreaRadiusEstimation = max(w, h) * scale.
                        loadDistance = static_cast<int>(
                            (std::max(portal->width, portal->height) * portalScale(*portal) * 1.4) / 16.0);
                    }
                    add(portal->destDimension, chunkOf(farPosOf(*portal)),
                        directLoadingDistance(loadDistance, distance),
                        ChunkLoader::Source::Portal);
                }

                // getGeneralPortalIndirectLoader, for the portals near the
                // player's image on the far side.
                for (const Game::Immersive::Portal* inner :
                     nearbyPortals(portal->destDimension, transformedPlayerPos, kIndirectRangeChunks, 32)) {
                    if (inner->Has(Game::Immersive::PortalFlag::Global)) {
                        const int radius = std::min(kLoadingRadiusCap, viewDistance / 3);
                        add(inner->destDimension, chunkOf(inner->TransformPoint(transformedPlayerPos)),
                            radius, ChunkLoader::Source::IndirectPortal);
                    } else {
                        add(inner->destDimension, chunkOf(farPosOf(*inner)),
                            cappedLoadingDistance(*inner, viewDistance / 4),
                            ChunkLoader::Source::IndirectPortal);
                    }
                }
            }
        }
#endif
        return loaders;
    }

    void IntegratedServer::EnsureGlobalPortals(Game::DimensionId dimension) {
#if ENABLE_IMMERSIVE_PORTALS
        if (!m_immersivePortals) return;
        using Game::Immersive::Portal;
        namespace PortalFlag = Game::Immersive::PortalFlag;
        const int  wrap  = m_config.worldWrapSize;
        // The Hush stays out of the Overworld→Nether→End ring: it is a
        // destination behind its own portal, not a floor of the stack.
        const bool stack = m_config.dimensionStack
                        && dimension != Game::DimensionId::Hush
                        && dimension != Game::DimensionId::TwilightForest
                        && dimension != Game::DimensionId::Aether;
        if (wrap <= 0 && !stack) return;

        const std::string dimName(Game::DimensionName(dimension));
        auto exists = [&](const std::string& tag) {
            bool found = false;
            m_immersivePortals->ForEachInDimension(dimension, [&](const Portal& p) {
                if (p.tag == tag) found = true;
            });
            return found;
        };
        auto global = [&](const std::string& tag) {
            Portal p;
            p.dimension = dimension;
            // Interactable matters for the stack: the way through a seam
            // is DUG. Standing on the Overworld's floor you stand on the
            // Nether's roof (cross-portal collision), and you mine that roof
            // through the surface under your feet; without the flag the
            // ray stopped at the seam and the roof could not be reached.
            p.flags = PortalFlag::Global | PortalFlag::Teleportable | PortalFlag::Visible |
                      PortalFlag::Interactable | PortalFlag::CrossPortalCollision |
                      PortalFlag::RenderPlayer;
            p.kind = Game::Immersive::PortalKind::Generic;
            p.tag  = tag;
            return p;
        };

        const double minY   = Game::DimensionMinY(dimension);
        const double maxY   = minY + Game::DimensionLogicalHeight(dimension);
        const double midY   = 0.5 * (minY + maxY);
        const double height = maxY - minY;

        // The wrap width of THIS dimension: the Nether is an eighth of the
        // Overworld, as its portal coordinates are.
        double width = static_cast<double>(wrap);
        if (dimension == Game::DimensionId::Nether) width = std::max(64.0, std::floor(wrap / 128.0) * 16.0);
        const double half = 0.5 * width;

        if (wrap > 0) {
            // East border faces −X (into the world), leads to the west
            // border; AddBiWay makes the west face (+X) from it. Its width
            // runs along +Z so that cross(W, up) is −X.
            const std::string tagX = "global:wrap_x:" + dimName;
            if (!exists(tagX)) {
                Portal p = global(tagX);
                p.origin = { half, midY, 0.0 };
                p.axisW  = { 0.0, 0.0, 1.0 };
                p.axisH  = { 0.0, 1.0, 0.0 };
                p.width  = width;
                p.height = height;
                p.destDimension = dimension;
                p.destination   = { -half, midY, 0.0 };
                m_immersivePortals->AddBiWay(p);
            }
            const std::string tagZ = "global:wrap_z:" + dimName;
            if (!exists(tagZ)) {
                Portal p = global(tagZ);
                p.origin = { 0.0, midY, half };
                p.axisW  = { -1.0, 0.0, 0.0 };   // cross(−X, up) = −Z, into the world
                p.axisH  = { 0.0, 1.0, 0.0 };
                p.width  = width;
                p.height = height;
                p.destDimension = dimension;
                p.destination   = { 0.0, midY, -half };
                m_immersivePortals->AddBiWay(p);
            }
        }

        if (stack) {
            // This dimension's floor onto the next one's ceiling, 1:1.
            // Overworld over Nether over End over Overworld.
            Game::DimensionId below;
            switch (dimension) {
                case Game::DimensionId::Overworld: below = Game::DimensionId::Nether;    break;
                case Game::DimensionId::Nether:    below = Game::DimensionId::End;       break;
                case Game::DimensionId::End:       below = Game::DimensionId::Overworld; break;
                case Game::DimensionId::Hush:
                case Game::DimensionId::TwilightForest:
                case Game::DimensionId::Aether:    return;   // excluded above; never reached
            }
            const std::string tag = "global:stack:" + dimName;
            if (!exists(tag)) {
                const double extent = wrap > 0 ? static_cast<double>(wrap) : 200000.0;
                const double belowTop = Game::DimensionMinY(below) + Game::DimensionLogicalHeight(below);
                Portal p = global(tag);
                p.origin = { 0.0, minY, 0.0 };
                p.axisW  = { 1.0, 0.0, 0.0 };
                p.axisH  = { 0.0, 0.0, -1.0 };   // cross(+X, −Z) = +Y: the front is above
                p.width  = extent;
                p.height = extent;
                p.destDimension = below;
                p.destination   = { 0.0, belowTop, 0.0 };
                const auto frontId = m_immersivePortals->AddBiWay(p);
                // The reverse is `below`'s CEILING seam. Under a dimension
                // with a bedrock roof (the Nether) it is only ever seen
                // through a hole dug in that roof, and it must be seen for
                // the way up to be dug. Under an open sky it would replace
                // the whole sky with the underside of the world above —
                // the End's black void over the Overworld, the Nether's
                // obsidian floor over the End — so it is not drawn there,
                // and it does not collide either (an unseen surface must not
                // be an invisible ceiling over the sky). The way up stays
                // open only where the floor above is the End's void: flying
                // up out of the Overworld lands in open air. Up out of the
                // End would land inside the Nether's obsidian floor, so that
                // ceiling is inert — the Nether is entered by digging down
                // from the Overworld, the End by falling out of the Nether
                // or flying up out of the Overworld.
                if (frontId != Game::Immersive::kInvalidPortalId && !Game::DimensionHasCeiling(below)) {
                    if (const Portal* front = m_immersivePortals->Get(frontId)) {
                        if (const Portal* back = m_immersivePortals->Get(front->reversePortalId)) {
                            Portal ceiling = *back;
                            ceiling.flags &= ~(PortalFlag::Visible | PortalFlag::Interactable |
                                               PortalFlag::CrossPortalCollision);
                            const bool floorAboveIsVoid = (dimension == Game::DimensionId::End);
                            if (!floorAboveIsVoid) ceiling.flags &= ~PortalFlag::Teleportable;
                            m_immersivePortals->Update(ceiling);
                        }
                    }
                }
            }
        }
        Log::Info("[ImmersivePortals] Global portals ensured for %s (wrap %d, stack %s)",
                  dimName.c_str(), wrap, stack ? "on" : "off");
#else
        (void)dimension;
#endif
    }

    // ── "No ambient occlusion" boxes ──────────────────────────────────────

    namespace {
        std::string AoRegionsPath(IntegratedServer& server) {
            ServerLevel* overworld = server.GetLevel(Game::DimensionId::Overworld);
            if (!overworld) return {};
            const std::string& root = overworld->Config().savePath;
            if (root.empty()) return {};
            return (std::filesystem::path(root) / "data" / "ao_regions.json").string();
        }
    }

    void IntegratedServer::AddAoRegion(Game::DimensionId dimension, const glm::ivec3& a, const glm::ivec3& b) {
        AoRegion r;
        r.dimension = dimension;
        r.min = glm::min(a, b);
        r.max = glm::max(a, b);
        m_aoRegions.push_back(r);
        BroadcastAoRegions(dimension);
        SaveAoRegions();
    }

    size_t IntegratedServer::RemoveAoRegionsAt(Game::DimensionId dimension, const glm::ivec3& pos) {
        const size_t before = m_aoRegions.size();
        m_aoRegions.erase(std::remove_if(m_aoRegions.begin(), m_aoRegions.end(), [&](const AoRegion& r) {
            return r.dimension == dimension &&
                   pos.x >= r.min.x && pos.x <= r.max.x &&
                   pos.y >= r.min.y && pos.y <= r.max.y &&
                   pos.z >= r.min.z && pos.z <= r.max.z;
        }), m_aoRegions.end());
        const size_t removed = before - m_aoRegions.size();
        if (removed) { BroadcastAoRegions(dimension); SaveAoRegions(); }
        return removed;
    }

    void IntegratedServer::SendAoRegions(ServerConnection& connection, Game::DimensionId dimension) const {
        Network::AoRegionsS2CPacket packet;
        packet.dimensionId = static_cast<int8_t>(Game::DimensionToRaw(dimension));
        for (const AoRegion& r : m_aoRegions) {
            if (r.dimension == dimension) packet.boxes.push_back({r.min, r.max});
        }
        connection.SendPacketIn(dimension, static_cast<uint8_t>(Network::PacketId::AoRegionsS2C),
                                Network::Serialization::Serialize(packet));
    }

    void IntegratedServer::BroadcastAoRegions(Game::DimensionId dimension) const {
        if (!m_sessionManager) return;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (!session || !session->LoadsDimension(dimension)) continue;
            if (auto* conn = session->GetConnection()) SendAoRegions(*conn, dimension);
        }
    }

    void IntegratedServer::LoadAoRegions() {
        m_aoRegions.clear();
        const std::string path = AoRegionsPath(*this);
        if (path.empty()) return;
        std::ifstream in(path);
        if (!in.is_open()) return;
        try {
            nlohmann::json j; in >> j;
            for (const auto& e : j.value("regions", nlohmann::json::array())) {
                AoRegion r;
                r.dimension = Game::DimensionFromRaw(e.value("dimension", 0));
                const auto mn = e.at("min"), mx = e.at("max");
                r.min = { mn.at(0).get<int>(), mn.at(1).get<int>(), mn.at(2).get<int>() };
                r.max = { mx.at(0).get<int>(), mx.at(1).get<int>(), mx.at(2).get<int>() };
                m_aoRegions.push_back(r);
            }
            Log::Info("[AoRegions] Loaded %zu box(es) from %s", m_aoRegions.size(), path.c_str());
        } catch (const std::exception& e) {
            Log::Warning("[AoRegions] Could not read %s: %s", path.c_str(), e.what());
        }
    }

    void IntegratedServer::SaveAoRegions() const {
        const std::string path = AoRegionsPath(const_cast<IntegratedServer&>(*this));
        if (path.empty()) return;
        nlohmann::json j;
        j["regions"] = nlohmann::json::array();
        for (const AoRegion& r : m_aoRegions) {
            j["regions"].push_back({
                {"dimension", Game::DimensionToRaw(r.dimension)},
                {"min", {r.min.x, r.min.y, r.min.z}},
                {"max", {r.max.x, r.max.y, r.max.z}},
            });
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path);
        if (out.is_open()) out << j.dump(2);
    }

    void IntegratedServer::SaveImmersivePortals() {
        SaveAoRegions();
#if ENABLE_IMMERSIVE_PORTALS
        if (m_immersivePortals) m_immersivePortals->Save();
#endif
#if ENABLE_PORTAL_GUN
        Game::Portal::ServerRegistry().Save();
#endif
    }

    // redstone_plus: settle the redstone that crosses this chunk's borders now
    // that it is resident. Neither vanilla nor this engine re-evaluates
    // redstone on load, and normally nothing needs it — but a dust network
    // that spans a chunk border can be evaluated while the far chunk is still
    // streaming in (the evaluator stops at unloaded chunks), and without
    // decay the far half then keeps its saved strength for good: measured
    // 2026-09-11, the Snake clock's return row half on and half off at x =
    // -368/-369, the oscillator latched after one step. Every component or
    // wire on a border whose neighbour chunk is loaded gets vanilla's
    // neighbour update, on both sides of the border, once. Redstone only:
    // sand, water and everything else stay exactly as loaded.
    void IntegratedServer::SettleRedstoneBorders(ServerLevel& level, Game::Math::ChunkPos cp) {
        Game::World* world = level.World();
        if (!world) return;
        auto chunk = world->GetLoadedChunk(cp.x, cp.z);
        if (!chunk) return;
        const bool west  = world->IsChunkLoaded(cp.x - 1, cp.z), east  = world->IsChunkLoaded(cp.x + 1, cp.z);
        const bool north = world->IsChunkLoaded(cp.x, cp.z - 1), south = world->IsChunkLoaded(cp.x, cp.z + 1);
        if (!west && !east && !north && !south) return;
        const int x0 = cp.x * 16, z0 = cp.z * 16;
        const auto consider = [&](int x, int y, int z, int qx, int qy, int qz) {
            const Game::BlockState s = world->GetBlockState(x, y, z);
            const Game::BlockID id = s.Block();
            if (id != Game::BlockID::RedstoneWire && !Game::IsRedstoneComponent(id)) return;
            world->UpdateNeighborsAt(glm::ivec3{x, y, z}, Game::BlockID::Air);       // the cell across hears it
            world->UpdateNeighborsAt(glm::ivec3{qx, qy, qz}, Game::BlockID::Air);    // ...and this cell re-evaluates
        };
        for (size_t si = 0; si < chunk->sections.size(); ++si) {
            const auto& sec = chunk->sections[si];
            if (!sec || sec->IsAllAir()) continue;
            const int y0 = Game::World::MIN_Y + static_cast<int>(si) * 16;
            for (int y = y0; y < y0 + 16; ++y) {
                for (int i = 0; i < 16; ++i) {
                    if (west)  consider(x0, y, z0 + i, x0 - 1, y, z0 + i);
                    if (east)  consider(x0 + 15, y, z0 + i, x0 + 16, y, z0 + i);
                    if (north) consider(x0 + i, y, z0, x0 + i, y, z0 - 1);
                    if (south) consider(x0 + i, y, z0 + 15, x0 + i, y, z0 + 16);
                }
            }
        }
    }

    void IntegratedServer::ProcessAsyncChunkResults() {
        PROFILE_ZONE;
        // Only process if async chunk loading is enabled
        if (!m_config.enableAsyncChunkLoading) {
            return;
        }

        auto& resultQueue = Threading::ServerWorkerPool::GetChunkGenResultQueue();

        // Bounded by the tick's own deadline, MC-style. This used to drain the
        // WHOLE queue every tick with the comment "let the send rate be the
        // throttle" — but the per-result work is not just a send: it scans the
        // chunk for portals and reads its entity file off disk. On a
        // freshly-generated world, where hundreds of results land in one burst,
        // that produced a single 2,475 ms tick, which was the largest spike in
        // the whole capture.
        //
        // MC bounds the equivalent work with haveTime() — an absolute deadline
        // at the start of the next tick (MinecraftServer.java:868), so a tick
        // already over budget does none of it. Nothing is lost: unprocessed
        // results stay queued and are picked up next tick.
        //
        // FLOOR, then deadline. The deadline alone was not safe: during mass
        // world generation the tick is routinely already over budget by the
        // time this runs, which would drop delivery to ONE chunk per tick and
        // turn a 1,057-chunk world load into a ~53-second wait. The floor
        // guarantees forward progress at ~160 chunks/second regardless, and the
        // deadline still stops a burst from producing the 2,475 ms tick this
        // bound exists to prevent.
        //
        // ~2.3 ms measured per result on a real world (portal scan + entity
        // file): 16 is ~37 ms worst case for the floor. Raised from 8 for the
        // simulation ring beyond the view distance — 20k+ chunks that stream
        // in at whatever this floor allows once the tick is busy (measured
        // 2026-09-10: ~500 chunks/s at 8, a 128-chunk ring took two minutes).
        constexpr int kMinResultsPerTick = 16;
        int resultsProcessed = 0;
        const bool haveDeadline = m_tickDeadline.time_since_epoch().count() != 0;

        // Chunks registered with their level's light engine this call: their
        // borders are reconciled with the neighbours below, before any of them
        // is sent (the packet then carries final light).
        std::vector<std::pair<ServerLevel*, Game::Math::ChunkPos>> newlyLit;

        Network::ChunkGenResult result;
        while (resultQueue.try_pop(result)) {
            // The queue is shared by every dimension, so the result's own
            // stamp is the only thing that says which level it belongs to.
            // A level torn down while its job was in flight leaves nothing to
            // file the result against — drop it rather than defaulting to the
            // overworld, which would push a Nether chunk to overworld players.
            ServerLevel* level = GetLevel(result.dimension);
            if (!level) {
                Log::Debug("Discarding chunk result for '%s' (%d, %d): that level is gone",
                           std::string(Game::DimensionName(result.dimension)).c_str(),
                           result.position.x, result.position.z);
                continue;
            }

            if (result.success && result.chunk) {
                // Chunk is already in ChunkProvider cache (worker called GetChunk -> CompleteChunkLoad)
                // Update status tracking
                {
                    PROFILE_ZONE_N("ChunkResult.MarkReady");
                    if (level->Status()) {
                        level->Status()->MarkChunkReady(result.position);
                    }
                }

                // Record any portal blocks this chunk holds, one index per
                // family. This is the only moment they can be found cheaply:
                // the scan rejects almost every section on a palette-
                // membership test, it runs once per chunk per session, and
                // after this the chunk may unload and never be walked again.
                //
                // It is also what makes the RETURN trip work. A portal you
                // built is remembered even after its chunk unloads behind you,
                // so coming back links to it instead of building a second one
                // a few blocks away — see NetherPortalIndex.hpp.
                {
                    PROFILE_ZONE_N("ChunkResult.PortalScan");
                    for (const Game::PortalFamilyId family : Game::kAllPortalFamilies) {
                        level->Portals(family).NoteChunkLoaded(result.position, *result.chunk);
                    }
                    // Villages' points of interest (beds, job sites, bells):
                    // MC's checkConsistencyWithBlocks, once per chunk.
                    level->Poi().NoteChunkLoaded(result.position, *result.chunk);
                    // The Twilight Forest pools, which are not a family.
                    TwilightPortalIndex(level->Dimension())
                        .NoteChunkLoaded(result.position, *result.chunk);
                    // Hush lighthouse lamps not yet told where the nearest
                    // Aurelith is: queued for LighthouseGuide::Tick.
                    LighthouseGuide::NoteChunkLoaded(*level, result.position, *result.chunk);
                    // Aurelith (AurelithCities.hpp): a Heart registers its
                    // city; an awakened city's dim lights come on as its
                    // chunks arrive; an awakening's wave takes them in.
                    if (auto* cities = level->Aurelith()) cities->OnChunkLoaded(result.position, *result.chunk);
#if ENABLE_IMMERSIVE_PORTALS
                    // Immersive portals: clear vanilla blocks out of recorded
                    // frames, adopt vanilla portals that lost their records.
                    if (m_netherPortalGeneration) m_netherPortalGeneration->OnChunkLoaded(*level, result.position);
#endif
                }
                if (Game::RedstonePlus::Enabled()) {
                    if (level->redstoneWarmup) level->redstoneSettleLater.push_back(result.position);
                    else SettleRedstoneBorders(*level, result.position);
                }

                // The level light engine takes the chunk (its own light came
                // from the worker, ChunkProvider::CompleteChunkLoad); the
                // border pass runs after this loop.
                if (Game::World* world = level->World()) {
                    if (auto* light = world->Light()) {
                        PROFILE_ZONE_N("ChunkResult.LightRegister");
                        light->AddChunk(result.chunk);
                        newlyLit.emplace_back(level, result.position);
                    }
                }

                // The chunk's entities come back with it. Before the chunk is
                // queued to the client, so a cow and the ground it stands on
                // arrive within a frame of each other.
                {
                    PROFILE_ZONE_N("ChunkResult.EntityLoad");
                    if (level->Entities()) level->Entities()->RequestLoad(result.position);
                }

                // MC ChunkMap.onChunkReadyToSend: the chunk PUSHES itself to
                // every player already tracking it. Nobody polls for it, and no
                // session keeps a "waiting for this chunk" list — the tracking
                // view is the only membership test.
                {
                    PROFILE_ZONE_N("ChunkResult.MarkPending");
                    if (m_sessionManager) {
                        m_sessionManager->ForEachSessionWatching(
                            result.dimension, result.position,
                            [&](PlayerSession& session) {
                                session.MarkChunkPendingToSend(result.dimension, result.position);
                            });
                    }
                }

                Log::Debug("Async chunk ready (%d, %d)",
                         result.position.x, result.position.z);
            } else {
                // Mark as failed so it can be retried
                if (level->Status()) {
                    level->Status()->SetChunkStatus(result.position, Server::ChunkStatus::EMPTY);
                }
                // Erasing from pendingChunkLoads below is the whole retry
                // mechanism: the chunk is still inside somebody's tracking view,
                // so RetryFailedChunkLoads re-requests it on a later tick.
                if (m_sessionManager) {
                    m_sessionManager->ForEachSessionWatching(
                        result.dimension, result.position,
                        [&](PlayerSession&) {
                            level->failedChunkLoads.insert(result.position);
                        });
                }
                Log::Warning("Async chunk load failed for (%d, %d): %s",
                           result.position.x, result.position.z,
                           result.errorMessage.c_str());
            }

            // Remove from pending list
            level->pendingChunkLoads.erase(result.position);
            level->generationDelivered.erase(result.position);   // its conversion has landed
            level->generationWaiting.erase(result.position);
            resultsProcessed++;

            // MC haveTime(): stop once this tick's deadline has passed and let
            // the rest wait — but only after the floor above has been met, so a
            // tick that is already late still delivers chunks.
            if (resultsProcessed >= kMinResultsPerTick && haveDeadline &&
                std::chrono::steady_clock::now() >= m_tickDeadline) {
                break;
            }
        }

        // Border reconciliation for the chunks just registered: one light
        // run per level. The new chunks have not been sent yet, so their own
        // affected sections need no update packet (their ChunkDataS2C, built
        // later this tick, carries the result) — only the neighbours' do, and
        // those go out with the next FlushLightUpdates. A new chunk whose
        // light the pass moved still gets a new stamp and a save mark.
        if (!newlyLit.empty()) {
            PROFILE_ZONE_N("ChunkResult.LightBorders");
            std::unordered_set<ServerLevel*> levelsRun;
            for (const auto& [lvl, pos] : newlyLit) {
                if (!levelsRun.insert(lvl).second) continue;
                if (Game::World* world = lvl->World()) {
                    if (auto* light = world->Light()) light->RunUpdates();
                }
            }
            for (const auto& [lvl, pos] : newlyLit) {
                Game::World* world = lvl->World();
                auto* light = world ? world->Light() : nullptr;
                if (!light) continue;
                if (light->DropAffectedSectionsOf(pos)) {
                    if (auto chunk = world->GetLoadedChunk(pos.x, pos.z)) chunk->BumpModStamp();
                    if (auto* provider = world->GetChunkProvider()) provider->MarkChunkForSave(pos);
                }
            }
        }

        m_stress.AddChunkResults(resultsProcessed);
        if (resultsProcessed > 0) {
            Log::Debug("Processed %d async chunk results this tick", resultsProcessed);
        }
    }

    void IntegratedServer::FlushLightUpdates(ServerLevel& level) {
        Game::World* world = level.World();
        Game::Lighting::LevelLightManager* light = world ? world->Light() : nullptr;
        if (!light) return;
        {
            PROFILE_ZONE_N("Light.Tick");
            light->RunUpdates();
        }
        const std::vector<int64_t> affected = light->TakeAffectedSections();
        if (affected.empty()) return;
        PROFILE_ZONE_N("Light.Broadcast");

        // Per chunk: which light sections to send (MC's per-layer BitSets,
        // reduced to one mask — both layers ride for each section).
        std::unordered_map<Game::Math::ChunkPos, uint32_t, Game::Math::ChunkPosHash> masks;
        for (int64_t key : affected) {
            const int li = Game::Lighting::SectionKey::Y(key) - Game::Lighting::kMinLightSectionY;
            if (li < 0 || li >= Game::Lighting::kLightSectionCount) continue;
            masks[Game::Math::ChunkPos{Game::Lighting::SectionKey::X(key), Game::Lighting::SectionKey::Z(key)}] |= 1u << li;
        }
        const Game::DimensionId dimension = level.Dimension();
        const bool hasSky = Game::DimensionHasSkyLight(dimension);
        auto* provider = world->GetChunkProvider();
        for (const auto& [pos, mask] : masks) {
            auto chunk = world->GetLoadedChunk(pos.x, pos.z);
            if (!chunk) continue;
            // Light is chunk content: a revisit must not revive a client's
            // retained copy from before this change, and the save must carry it.
            chunk->BumpModStamp();
            if (provider) provider->MarkChunkForSave(pos);
            const auto data = Network::Serialization::SerializeLightUpdate(pos.x, pos.z, chunk->light, mask, hasSky);
            SendToChunkWatchersAt(dimension, pos, Network::PacketId::LightUpdateS2C, data);
        }
    }


    // The OVERWORLD's, specifically — see the header. PumpChunkPipeline
    // deliberately does not use this: it has to reach all three.
    Game::MyTerrainGenerator* IntegratedServer::GetTerrainGenerator() const {
        ServerLevel* level = GetLevel(Game::DimensionId::Overworld);
        return level ? level->TerrainGenerator() : nullptr;
    }

    namespace {
        // Where each level's queued chunk work should be nearest to: every
        // session's own view centre in the level it stands in, plus every
        // portal far side, filed under the dimension that far side is in.
        // The simulation ring shares the player's centre, so it adds nothing.
        Threading::ChunkLoadAnchors CollectChunkLoadAnchors(
                const std::vector<std::shared_ptr<PlayerSession>>& sessions) {
            Threading::ChunkLoadAnchors anchors;
            for (const auto& session : sessions) {
                if (!session) continue;
                const Game::DimensionId here = Game::DimensionFromRaw(session->GetDimensionId());
                anchors[Game::DimensionSlot(here)].push_back(session->GetAnchorChunk());
                for (const ChunkLoader& loader : session->Loaders()) {
                    if (loader.source != ChunkLoader::Source::Player && !loader.IsSimulation()) {
                        anchors[Game::DimensionSlot(loader.dimension)].push_back(loader.Center());
                    }
                }
            }
            return anchors;
        }
    } // namespace

    void IntegratedServer::ServiceGenerationQueues(ServerLevel& level, Game::MyTerrainGenerator& gen) {
        PROFILE_ZONE_N("ServiceGenerationQueues");
        const Game::DimensionId dimension = level.Dimension();

        // MC DistanceManager.ticketDispatcher = ThrottlingChunkTaskDispatcher(
        // ..., 4): player loading tickets are added nearest-first, and at most
        // FOUR chunk positions per level are "in execution" — ticketed and not
        // yet ENTITY_TICKING. Each request's ticket is at the player ticket
        // level (31), so its whole 5x5 generates together; a slot frees when
        // that 5x5 is FULL (the library's release, TakeReleases) or the
        // request is dropped. A chunk already generated — most of a view once
        // its first clusters are done — passes through a slot at once.
        // This replaced 24 requests at level 33, each an isolated FULL chunk
        // with a pyramid of its own: 50 players flying apart generated ~14
        // chunks a second (stress test 2026-09-24). Giving the library every
        // chunk of a view at once was measured slower still (2026-08-30).
        static const size_t kMaxInFlight = [] {   // OBEY_INFLIGHT env overrides for tuning runs
            const char* e = std::getenv("OBEY_INFLIGHT"); return e ? (size_t)std::atoi(e) : (size_t)4; }();
        const auto now = std::chrono::steady_clock::now();

        // Nothing to do: no request or completion waiting, nothing the
        // backlog could issue, and the stall watchdog not due. This is called
        // on every pump of the pipeline — ~83,000 times a second under load
        // (profile, 2026-09-23: 17% of the server thread) — and almost every
        // call used to walk the full service path for nothing.
        const bool canIssue = !level.generationBacklog.empty() && level.generationInFlight < kMaxInFlight;
        const bool stallCheckDue = level.generationInFlight > 0 &&
                                   now - level.generationLastStallCheck >= std::chrono::seconds(1);
        if (!canIssue && !stallCheckDue && !gen.HasQueuedWork()) return;
        if (stallCheckDue) level.generationLastStallCheck = now;
        {
            std::vector<Game::Math::ChunkPos> fresh;
            gen.TakeRequests(fresh);
            level.generationWaiting.insert(fresh.begin(), fresh.end());
            if (!fresh.empty()) {
                // At the FRONT (the back is served first): until this tick's
                // or the next tick's sort places them, new requests queue
                // behind the ones already ordered nearest-first.
                level.generationBacklog.insert(level.generationBacklog.begin(), fresh.begin(), fresh.end());
                level.generationBacklogSorted = false;
            }
        }

        // Stall watchdog. A request the library has held for kStuckAfter with
        // nothing at all completing meanwhile is not slow, it is lost — a
        // task the port once dropped between scheduling and running kept its
        // request forever (fixed: ChunkMap::m_pendingTasksMutex); the
        // watchdog stays for whatever else can strand one. Give the request up (unpin, free its
        // in-flight slot, put it back on the backlog) so the pipeline keeps
        // moving, and say so, with the library's own state, so a log tells
        // the two apart. Before this, 24 lost requests froze a 50k-chunk
        // simulation ring for good (2026-09-10).
        constexpr auto kStuckAfter = std::chrono::seconds(8);
        if (stallCheckDue && !level.generationIssued.empty() &&
            now - level.generationLastCompletion > kStuckAfter) {
            std::vector<Game::Math::ChunkPos> stuck;
            for (const auto& [pos, issued] : level.generationIssued) {
                if (now - issued > kStuckAfter) stuck.push_back(pos);
            }
            if (!stuck.empty()) {
                const auto diag = gen.GetUnloadDiag();
                Log::Warning("[Generation] %zu request(s) held by the terrain library for >%llds with no "
                             "completion — reissuing (in flight %zu, backlog %zu, library holders %zu, "
                             "refHeld %zu, pinned %zu, aboveMax %zu, pendingUnload %zu)",
                             stuck.size(), static_cast<long long>(kStuckAfter.count()),
                             level.generationInFlight, level.generationBacklog.size(), gen.LibraryChunkCount(),
                             diag.refHeld, diag.pinned, diag.aboveMax, diag.pendingUnload);
                constexpr auto kQuarantine = std::chrono::seconds(120);
                for (const auto& pos : stuck) {
                    gen.UnpinConversion(pos);
                    gen.ReleaseGenerationRequest(pos);   // re-issued below with a fresh ticket
                    level.generationIssued.erase(pos);
                    if (level.generationInFlight > 0) --level.generationInFlight;
                    ++level.genStuck;
                    level.generationQuarantine[pos] = now + kQuarantine;
                    if (level.pendingChunkLoads.count(pos)) {
                        level.generationBacklog.push_back(pos);
                        level.generationBacklogSorted = false;
                    }
                }
                level.generationLastCompletion = now;    // one warning per stall, not one per tick
            }
        }

        if (!level.generationBacklog.empty() && level.generationInFlight < kMaxInFlight) {
            // Nearest to every player position IN THIS LEVEL — each session's
            // own view centre and any portal far side that lands here. This
            // used to be GetPlayerSession()'s anchor — session id 1 — whatever
            // level was being served. Player ids are connection ids from a
            // process-wide counter (ServerConnection::s_nextConnectionId), so
            // after a second world load in one run, or any connection ahead
            // of the local one, there was no session 1 and the anchor fell
            // back to chunk (0,0): after a /tp, a /dim or a portal the new
            // area generated in order of distance from the world ORIGIN — a
            // sweep across the view by chunk coordinates. Spawn, being near
            // (0,0), hid it. With a session 1, a portal far side (and every
            // other player's area) swept from wherever that player stood, in
            // the wrong level's coordinates.
            std::vector<Game::Math::ChunkPos> anchors;
            if (m_sessionManager) {
                anchors = std::move(CollectChunkLoadAnchors(m_sessionManager->GetAllSessions())
                                        [Game::DimensionSlot(dimension)]);
            }
            auto& bl = level.generationBacklog;
            // Nearest-first, consumed from the BACK: sort descending by
            // distance once, and again only when entries arrive or an
            // anchor has moved a few chunks. Entries nobody wants any more
            // (their load was cancelled) are skipped as they surface.
            bool moved = anchors.size() != level.generationBacklogAnchors.size();
            for (size_t i = 0; !moved && i < anchors.size(); ++i) {
                moved = std::max(std::abs(anchors[i].x - level.generationBacklogAnchors[i].x),
                                 std::abs(anchors[i].z - level.generationBacklogAnchors[i].z)) >= 4;
            }
            // At most once a tick: requests arrive all the time, and this ran
            // again on every PumpChunkPipeline call that saw one (several per
            // tick) — over the whole backlog, cancelled entries included.
            // Fresh entries wait unsorted at the front of the vector (served
            // last) until the next tick's sort.
            const bool sortDue = (!level.generationBacklogSorted || moved) &&
                                 level.generationBacklogSortTick != m_currentServerTick;
            if (sortDue) {
                // Cancelled requests leave the backlog here, physically —
                // skipping them only as they reached the front meant the dead
                // ones, which are far from everyone, sorted to the back and
                // were never reached: the backlog only ever grew.
                bl.erase(std::remove_if(bl.begin(), bl.end(), [&](const Game::Math::ChunkPos& p) {
                             return level.pendingChunkLoads.count(p) == 0;
                         }), bl.end());
                level.generationBacklogSortTick = m_currentServerTick;
                // One key per entry, then one sort: O(n·anchors + n log n).
                std::vector<std::pair<int64_t, Game::Math::ChunkPos>> keyed;
                keyed.reserve(bl.size());
                for (const auto& p : bl) {
                    int64_t best = anchors.empty() ? 0 : std::numeric_limits<int64_t>::max();
                    for (const auto& a : anchors) {
                        const int64_t dx = p.x - a.x, dz = p.z - a.z;
                        best = std::min(best, dx * dx + dz * dz);
                    }
                    keyed.emplace_back(best, p);
                }
                std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (size_t i = 0; i < keyed.size(); ++i) bl[i] = keyed[i].second;
                level.generationBacklogSorted = true;
                level.generationBacklogAnchors = std::move(anchors);
            }
            std::vector<Game::Math::ChunkPos> held;                       // quarantined: skip, keep
            while (!bl.empty() && level.generationInFlight < kMaxInFlight) {
                const auto pos = bl.back();
                bl.pop_back();
                if (level.pendingChunkLoads.count(pos) == 0) {             // cancelled meanwhile
                    level.generationDelivered.erase(pos);
                    continue;
                }
                if (level.generationDelivered.erase(pos)) continue;        // already on its way (ready)
                auto q = level.generationQuarantine.find(pos);
                if (q != level.generationQuarantine.end()) {
                    if (now < q->second) { held.push_back(pos); continue; }
                    level.generationQuarantine.erase(q);
                }
                if (gen.RequestChunkGeneration(pos)) {
                    ++level.generationInFlight;
                    level.generationIssued[pos] = now;
                } else {
                    ++level.genRefused;
                    level.pendingChunkLoads.erase(pos);
                    level.failedChunkLoads.insert(pos);
                }
            }
            // Quarantined entries go back under everything else (the back is
            // the front of the queue), so they surface again only once the
            // rest has been handed out.
            bl.insert(bl.begin(), held.begin(), held.end());
        }

        // Throttle slots let go (MC ticketsToRelease): the request's 5x5 is
        // FULL, or its ticket went. Only a request still on the books frees
        // one — a request the watchdog already gave up on must not free a
        // second.
        {
            std::vector<Game::Math::ChunkPos> released;
            gen.TakeReleases(released);
            if (!released.empty()) level.generationLastCompletion = now;
            for (const auto& pos : released) {
                if (level.generationIssued.erase(pos) && level.generationInFlight > 0) --level.generationInFlight;
            }
        }

        Game::World* world = level.World();
        Game::ChunkProvider* provider = world ? world->GetChunkProvider() : nullptr;
        Threading::ServerWorkerPool* pool = Threading::g_serverWorkerPool.get();
        // Conversion (~0.5 ms) on a worker, then into the game's cache and
        // the ordinary ChunkGenResult queue. The caller's pin keeps
        // processUnloads off the holder until `unpin` runs.
        const auto convert = [&](Game::Math::ChunkPos pos, minecraft::world::IChunk* lib,
                                 std::function<void()> unpin) {
            Game::MyTerrainGenerator* genp = &gen;
            pool->SubmitWorldIOJob([genp, provider, pool, dimension, pos, lib, unpin]() {
                std::shared_ptr<Game::Chunk> chunk = genp->ConvertCompletedChunk(lib, pos);
                if (chunk) chunk = provider->StoreChunkInCache(chunk);
                // The game owns it now (StoreChunkInCache marks it for saving,
                // and every later load reads that save first): the library's
                // copy of its blocks may go. Before the unpin, so the release
                // can never see this holder unpinned yet not handed off.
                if (chunk) genp->NoteHandedOff(pos);
                unpin();
                pool->SendChunkGenResult(dimension, pos, chunk, chunk != nullptr,
                                         chunk ? "" : "conversion failed");
            }, /*priority=*/1);
        };

        // MC ChunkMap.onChunkReadyToSend: a chunk someone is waiting for is
        // theirs as soon as its 3x3 is FULL, whichever request's ticket got it
        // there — the throttle paces tickets, never delivery. Without this a
        // request's 5x5 turned into one chunk for the game; the other 24 sat
        // FULL until their own requests came round, by which time a moving
        // player had often left them behind.
        {
            std::vector<Game::MyTerrainGenerator::Completion> ready;
            gen.TakeReady(ready);
            static const bool kNoReadyDelivery = std::getenv("OBEY_NO_READY_DELIVERY") != nullptr;   // A/B kill switch
            for (const auto& r : ready) {
                if (kNoReadyDelivery) { gen.UnpinReady(r.position); continue; }
                // Waiting for GENERATION (the disk had nothing) — never a chunk
                // being read from the game's own save, whatever the library
                // holds for it.
                const bool wanted = level.pendingChunkLoads.count(r.position) != 0 &&
                                    level.generationWaiting.count(r.position) != 0 &&
                                    level.generationIssued.count(r.position) == 0 &&
                                    level.generationDelivered.count(r.position) == 0;
                if (!wanted || !r.chunk || !provider || !pool) {
                    gen.UnpinReady(r.position);
                    continue;
                }
                level.generationDelivered.insert(r.position);
                ++level.genDeliveredReady;
                Game::MyTerrainGenerator* genp = &gen;
                const Game::Math::ChunkPos pos = r.position;
                convert(pos, r.chunk, [genp, pos]() { genp->UnpinReady(pos); });
            }
            // Entries whose load is over leave (they also leave when the load
            // result lands or the load is cancelled); this catches the rest.
            for (auto* set : {&level.generationDelivered, &level.generationWaiting}) {
                if (set->size() <= 4096 || set->size() <= 2 * level.pendingChunkLoads.size()) continue;
                for (auto it = set->begin(); it != set->end();) {
                    it = level.pendingChunkLoads.count(*it) ? std::next(it) : set->erase(it);
                }
            }
        }

        std::vector<Game::MyTerrainGenerator::Completion> done;
        gen.TakeCompletions(done);
        if (done.empty()) return;
        level.genCompleted += done.size();
        level.generationLastCompletion = now;
        for (const auto& c : done) {
            if (!c.chunk) ++level.genCompletedEmpty;
            // A failed or unwanted request lets go of its ticket now. A good
            // result keeps it while the chunk is in a player's view — MC's
            // PLAYER_LOADING ticket — and loses it when the chunk leaves every
            // view (CancelLoadIfUnwanted; MyTerrainGenerator::
            // SweepRequestTickets catches the rest). Releasing it here
            // freed each request's generation neighbourhood the moment it
            // finished, only for the next request next door to regenerate
            // most of it: ~9k holders a second freed and rebuilt, generation
            // down from ~430 to ~250 chunks a second (2026-09-23).
            if (!c.chunk || level.pendingChunkLoads.count(c.position) == 0) {
                gen.ReleaseGenerationRequest(c.position);
            }
            // The chunk being FULL does not free its throttle slot; its 5x5
            // being FULL does (the release above).
        }
        for (const auto& c : done) {
            // Nobody wants it any more — or it was already taken when the
            // library announced it ready.
            if (level.pendingChunkLoads.count(c.position) == 0 ||
                level.generationDelivered.count(c.position) != 0) {
                gen.UnpinConversion(c.position);
                continue;
            }
            if (!c.chunk || !provider || !pool) {
                gen.UnpinConversion(c.position);
                if (pool) pool->SendChunkGenResult(dimension, c.position, nullptr, false, "generation failed");
                continue;
            }
            // The pin taken when the request attached keeps processUnloads
            // off this holder until the job unpins.
            Game::MyTerrainGenerator* genp = &gen;
            const Game::Math::ChunkPos pos = c.position;
            level.generationDelivered.insert(pos);
            convert(pos, c.chunk, [genp, pos]() { genp->UnpinConversion(pos); });
        }
    }

    void IntegratedServer::PumpChunkPipeline(std::chrono::steady_clock::time_point deadline) {
        // Gathered once: the round-robin below revisits the same generators
        // until they are all idle, and TerrainGenerator() costs a dynamic_cast
        // per call.
        Game::MyTerrainGenerator* generators[Game::kDimensionCount] = {};
        ServerLevel*              levels[Game::kDimensionCount] = {};
        size_t count = 0;
        ForEachLevel([&](ServerLevel& level) {
            if (auto* gen = level.TerrainGenerator()) { levels[count] = &level; generators[count++] = gen; }
        });
        if (count == 0) return;

        PROFILE_ZONE_N("PumpChunkPipeline");

        // ── Ticket-driven generation (MC ChunkMap model) ──────────────────
        // Workers hand chunks that are not on disk to the generator's request
        // queue; here — the library's main thread — each becomes a ticket +
        // future. Completions (any thread) land in the generator's sink and
        // are converted to game chunks on a worker, then flow through the
        // ordinary ChunkGenResult queue. Nothing ever blocks on the library.
        for (size_t i = 0; i < count; ++i) {
            ServiceGenerationQueues(*levels[i], *generators[i]);
        }

        // MC BlockableEventLoop.managedBlock(() -> !haveTime()):
        //
        //     while (!condition.getAsBoolean()) {
        //         if (!this.pollTask()) this.waitForTasks();
        //     }
        //
        // The clock is re-checked before EVERY unit of work. That is the whole
        // point — one unit is bounded, the pipeline as a whole is not, and the
        // deadline is the only thing standing between "pump the pipeline" and
        // "stop ticking the server for five seconds".
        //
        // ROUND-ROBIN over every live dimension, not just one. Each generator
        // owns its own main-thread queue, and ServerWorkerPool::
        // ProcessChunkLoading calls the BLOCKING ChunkProvider::GetChunk, which
        // parks the worker until the server thread pumps THAT dimension's
        // queue. Pumping only one generator therefore does not merely delay a
        // chunk — it wedges however many workers are parked on the others, and
        // a wedged pool stops loading chunks for every dimension including the
        // one being pumped.
        //
        // `idle` counts CONSECUTIVE generators that had nothing to do, so the
        // loop exits only once a full round produced no work — MC's
        // waitForTasks(), generalised to N queues. Any generator doing work
        // resets it, which is what keeps a busy overworld from being abandoned
        // because the End had nothing queued.
        //
        // FLOOR, then deadline — the same shape as ProcessAsyncChunkResults.
        // At a high tick rate (/tick rate 400) the per-tick budget is a couple
        // of milliseconds and the tick itself uses it, so a pure deadline pumped
        // nothing at all and every generation request the library held stayed
        // held (measured 2026-09-10: a 50k-chunk simulation ring froze at 24
        // in flight). A few dozen task hops per tick is bounded work and keeps
        // the pipeline alive whatever the rate.
        constexpr size_t kMinPumpsPerTick = 64;
        size_t idle = 0;
        size_t next = 0;
        size_t pumps = 0;
        while (idle < count && (pumps < kMinPumpsPerTick || std::chrono::steady_clock::now() < deadline)) {
            if (generators[next]->PumpOneTask()) {
                idle = 0;
            } else {
                ++idle;
            }
            ++pumps;
            next = (next + 1) % count;
        }
    }

    void IntegratedServer::CancelLoadIfUnwanted(Game::DimensionId dim, Game::Math::ChunkPos pos,
                                                const PlayerSession* excluding,
                                                const std::vector<std::shared_ptr<PlayerSession>>& sessions) {
        ServerLevel* L = GetLevel(dim);
        if (!L) return;
        if (L->Keeper() && L->Keeper()->Kept(pos)) return;
        for (const auto& other : sessions) {
            if (other && other.get() != excluding && other->KeepsLoaded(dim, pos)) return;
        }
        // The chunk has left every player's view: its generation ticket goes
        // NOW, as MC's PLAYER_LOADING ticket does the moment the chunk is out
        // of every player's range (DistanceManager.PlayerTicketTracker
        // .onLevelChange -> removeTicket). The library's holders around it
        // then unload once nothing else's ticket reaches them. Generating
        // right now: that cancels the library's work on it, as MC cancels a
        // chunk whose ticket goes (the failed completion still comes back and
        // frees the in-flight slot). Already generated: the game keeps its
        // chunk under MC's own rule (UnloadUnwatchedChunks); the ticket used
        // to be held for as long as the game kept the chunk, which with the
        // old two-minute residency meant a flying player's whole trail of
        // library chunks. Idempotent for a chunk that holds no ticket.
        if (auto* gen = L->TerrainGenerator()) gen->ReleaseGenerationRequest(pos);

        // A load still in flight for a chunk nobody watches any more is pure
        // waste — and after a far teleport that is most of the queue:
        // measured 2026-08-29, ~1,500 chunks of the OLD area kept generating
        // for 30 s after the player left. MC has no equivalent because its
        // tickets ARE the request: drop the ticket and the chunk holder's
        // future is cancelled.
        auto pending = L->pendingChunkLoads.find(pos);
        if (pending == L->pendingChunkLoads.end()) return;
        L->pendingChunkLoads.erase(pending);
        L->generationWaiting.erase(pos);
        L->generationDelivered.erase(pos);
        if (Threading::g_serverWorkerPool) {
            Threading::g_serverWorkerPool->CancelChunkJobs(dim, pos);
        }
        ++m_abandonedLoadsCancelled;
    }

    void IntegratedServer::QueueAbandonedChunks(std::vector<DimChunkKey> keys) {
        if (keys.empty()) return;
        std::lock_guard<std::mutex> lock(m_abandonedMutex);
        if (m_abandonedChunks.empty()) {
            m_abandonedChunks = std::move(keys);
        } else {
            m_abandonedChunks.insert(m_abandonedChunks.end(), keys.begin(), keys.end());
        }
    }

    void IntegratedServer::ProcessWatchSetChanges() {
        // /control: a controller's anchor follows the controlled player;
        // pinned before the loaders below are computed from it.
        if (m_remoteControl) m_remoteControl->Tick();
        PROFILE_ZONE;
        if (!m_sessionManager) return;

        // Hand the generators the requests the workers have queued and take
        // their completions — tickets in, results out, no generation work.
        // MC's tick does the same share of chunk work: ServerChunkCache.tick
        // runs ONE distance-manager pass (here TickLibrary, in the
        // maintenance phase, which also attaches these requests), and the
        // pipeline itself — MainThreadExecutor.pollTask — runs only in the
        // time between ticks (the loop's pump window, PumpChunkPipeline).
        // This used to pump here too, for up to a whole tick on top of that
        // window, so the tick's own work waited behind generation callbacks.
        ForEachLevel([this](ServerLevel& level) {
            if (auto* gen = level.TerrainGenerator()) ServiceGenerationQueues(level, *gen);
        });

        // Re-request anything whose load came back failed. Normally a no-op.
        // Per level, because the failure and the retry both have to name the
        // dimension the chunk was wanted in.
        ForEachLevel([this](ServerLevel& level) {
            if (level.failedChunkLoads.empty()) return;
            auto failed = std::move(level.failedChunkLoads);
            level.failedChunkLoads.clear();
            level.genRetried += failed.size();
            for (const auto& pos : failed) {
                RequestChunkLoad(level.Dimension(), pos, 0);
            }
        });

        // Refresh the positions the worker pool prioritises against. MC
        // re-evaluates its queue level per poll for the same reason: a request
        // burst ordered by where the player stood when it was issued is wrong
        // by the time the workers get to the far half of it.
        //
        // Per dimension: a portal far side's centre is in the far level's
        // coordinates, and measuring one level's chunks against another's
        // positions is what turned nearest-first into a sweep across the map.
        Threading::SetServerChunkLoadAnchors(CollectChunkLoadAnchors(m_sessionManager->GetAllSessions()));

        // Update every player's tracking view and act on the difference. This
        // is MC ChunkMap.move -> updateChunkTracking -> applyChunkTrackingView:
        //
        //   enter -> markChunkPendingToSend if the chunk is loaded, otherwise
        //            ask for it and let the completion push it later
        //   leave -> dropChunk (unload packet if it had been sent)
        //
        // Called unconditionally every tick: UpdateChunkTracking early-outs
        // when neither the centre chunk nor the view distance changed, so a
        // player standing still — or moving within one chunk — costs one
        // comparison. That replaced a per-tick scan of every pending chunk
        // (up to 1369 IsChunkLoaded calls per session).
        auto sessions = m_sessionManager->GetAllSessions();

        // Chunks a dimension change or a disconnect dropped without the
        // per-chunk leave below (QueueAbandonedChunks). Their loads are
        // cancelled here, on this thread, unless someone else still wants them.
        {
            std::vector<DimChunkKey> abandoned;
            {
                std::lock_guard<std::mutex> lock(m_abandonedMutex);
                abandoned.swap(m_abandonedChunks);
            }
            for (const DimChunkKey& key : abandoned) {
                CancelLoadIfUnwanted(key.Dimension(), key.Pos(), nullptr, sessions);
            }
        }
        for (const auto& session : sessions) {
            if (!session) continue;

            // Library-side view tickets (MC player tickets) are OFF: see
            // MyTerrainGenerator::RequestChunkGeneration. Kept behind a flag for
            // when library unloading is re-enabled (they are what expires the
            // area behind the player).
            static constexpr bool kLibraryViewTickets = false;
            if (kLibraryViewTickets) {
                ServerLevel& L = LevelOf(*session);
                ForEachLevel([&](ServerLevel& other) {
                    auto* gen = other.TerrainGenerator();
                    if (!gen) return;
                    if (&other == &L) gen->SetViewTicket(session->GetPlayerId(), session->GetAnchorChunk(),
                                                         std::min(session->GetViewDistance() + 1, 33));
                    else              gen->ClearViewTicket(session->GetPlayerId());
                });
            }

            // Every chunk the diff reports names its dimension: the session's
            // loaders span the world it stands in and every portal far side
            // near it. A far side's level is built on demand — a portal into
            // the Nether is what brings the Nether into being, exactly as
            // walking through one did before.
            session->UpdateChunkTracking(
                ComputeChunkLoaders(*session),
                [&](Game::DimensionId dim, Game::Math::ChunkPos pos) {
                    ServerLevel* L = GetOrCreateLevel(dim);
                    if (!L || !L->World()) return;
                    // MC markChunkPendingToSend: getChunkToSend() returns null
                    // for a chunk that is not loaded, and the call quietly does
                    // nothing. Ours asks the world the same question.
                    if (L->World()->IsChunkLoaded(pos.x, pos.z)) {
                        // Claim its entities. This branch NEVER goes through
                        // RequestChunkLoad, so it was the one route by which a
                        // chunk could become visible to a player without its
                        // entities/*.mca entry ever being read — and it is the
                        // route the SPAWN chunks take, the player's own chunk
                        // among them, because those are already resident by the
                        // time the session starts tracking them.
                        if (L->Entities()) L->Entities()->RequestLoad(pos);
                        session->MarkChunkPendingToSend(dim, pos);
                    } else {
                        RequestChunkLoad(dim, pos, 0);
                    }
                },
                [&](Game::DimensionId dim, Game::Math::ChunkPos pos, bool stillLoaded) {
                    session->DropChunk(dim, pos);
                    // Left the view but still inside the simulation ring: the
                    // client gets its unload, the server keeps the chunk.
                    if (stillLoaded) return;

                    // The session's own set is not updated until after this
                    // callback, so it still reports the chunk as watched —
                    // only OTHER sessions count.
                    CancelLoadIfUnwanted(dim, pos, session.get(), sessions);
                },
                [&](Game::DimensionId dim, Game::Math::ChunkPos pos) {
                    // Entered the simulation ring only: make sure it is
                    // resident (and its entities with it) so it ticks; nothing
                    // is queued for the client.
                    ServerLevel* L = GetOrCreateLevel(dim);
                    if (!L || !L->World()) return;
                    if (L->World()->IsChunkLoaded(pos.x, pos.z)) {
                        if (L->Entities()) L->Entities()->RequestLoad(pos);
                    } else {
                        RequestChunkLoad(dim, pos, 0);
                    }
                });
        }
    }

    void IntegratedServer::ForceTimeSync() {
        if (!m_sessionManager) return;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (session && session->GetConnection()) {
                session->GetConnection()->SendCurrentTimeUpdate();
            }
        }
    }

    void IntegratedServer::UnloadUnwatchedChunks() {
        PROFILE_ZONE;
        if (!m_sessionManager) return;

        // Snapshot once and reuse across levels — the walk below is per level
        // but the player list is not.
        const auto sessions = m_sessionManager->GetAllSessions();

        size_t unloaded = 0;

        // Every level, including the ones with no players: an abandoned Nether
        // is precisely the one whose chunks most need dropping.
        ForEachLevel([&](ServerLevel& level) {
            Game::World* world = level.World();
            if (!world) return;

            // Per level, not shared across the sweep: these ids go only to this
            // dimension's players, and item/orb ids are allocated per level from
            // the same base — so merging two levels' removals would retire an
            // Overworld item on the strength of a Nether one vanishing.
            std::vector<int32_t> removedItems;

            auto* chunkProvider = world->GetChunkProvider();
            if (!chunkProvider) return;

            // Iterate only actually loaded chunks instead of scanning a huge grid
            const Game::DimensionId dimension = level.Dimension();
            auto anyoneTracking = [&sessions, dimension](Game::Math::ChunkPos pos) {
                for (const auto& session : sessions) {
                    if (!session) continue;
                    if (session->KeepsLoaded(dimension, pos)) return true;
                }
                return false;
            };

            // MC keeps a chunk in memory while its holder's ticket level is
            // at most MAX_LEVEL (44). A player's PLAYER_LOADING tickets sit at
            // level 31 on every chunk within their view distance (DistanceManager
            // .PlayerTicketTracker, Chebyshev), and the level rises by one per
            // chunk beyond it — so a chunk goes once it is more than 44 - 31 = 13
            // chunks outside every player's view, and not before. The same for
            // simulation and portal loaders (PLAYER_SIMULATION and PORTAL
            // tickets). There is no timer: this replaced a two-minute residency
            // grace (and a 12,000-chunk cap that cut it short), which kept a
            // flying player's whole trail loaded — and, through its generation
            // tickets, the terrain library's chunks around it.
            constexpr int kUnloadMargin = 44 - 31;   // MC ChunkLevel.MAX_LEVEL - DistanceManager.PLAYER_TICKET_LEVEL
            std::vector<std::pair<Game::Math::ChunkPos, int>> reach;   // centre, radius + margin
            for (const auto& session : sessions) {
                if (!session) continue;
                for (const ChunkLoader& loader : session->Loaders()) {
                    if (loader.dimension == dimension) {
                        reach.emplace_back(loader.Center(), loader.Radius() + kUnloadMargin);
                    }
                }
            }
            ChunkKeeper* keeper = level.Keeper();
            const auto wanted = [&](Game::Math::ChunkPos pos) {
                for (const auto& [centre, radius] : reach) {
                    if (std::max(std::abs(pos.x - centre.x), std::abs(pos.z - centre.z)) <= radius) return true;
                }
                return anyoneTracking(pos) || (keeper && keeper->Kept(pos));
            };

            std::vector<Game::Math::ChunkPos> unloadedForMobs;

            // ── 1. Scan a slice ───────────────────────────────────────────
            // MC learns a chunk is unwanted from the level change itself
            // (ChunkMap.updateChunkScheduling -> toDrop). Ours asks: every
            // loaded chunk once a second, a twentieth of the list per tick —
            // one pass over every chunk against every session each tick was
            // 240-330 ms (stress test 2026-09-23).
            if (level.unloadScanCursor >= level.unloadScan.size()) {
                level.unloadScan = chunkProvider->GetLoadedChunkPositions();
                level.unloadScanCursor = 0;
            }
            const size_t slice = std::max<size_t>(64, (level.unloadScan.size() + 19) / 20);
            const size_t scanEnd = std::min(level.unloadScan.size(), level.unloadScanCursor + slice);
            for (size_t i = level.unloadScanCursor; i < scanEnd; ++i) {
                const Game::Math::ChunkPos pos = level.unloadScan[i];
                if (wanted(pos)) continue;
                if (level.unloadQueued.insert(pos).second) level.unloadQueue.push_back(pos);
            }
            level.unloadScanCursor = scanEnd;

            // ── 2. Unload in the tick's spare time ────────────────────────
            // MC ChunkMap.processUnloads:
            //     int minimal = Math.max(0, this.unloadQueue.size() - 2000);
            //     while ((minimal > 0 || haveTime.getAsBoolean()) && (task = poll()) != null) ...
            // Only a queue past 2,000 forces work into a tick that has none
            // to spare.
            size_t mustProcess = level.unloadQueue.size() > 2000 ? level.unloadQueue.size() - 2000 : 0;
            const bool haveDeadline = m_tickDeadline.time_since_epoch().count() != 0;
            // MC PersistentEntitySectionManager.processChunkUnload: a chunk
            // whose entities cannot be stored yet — read Pending, or never
            // read while it holds live ones — stays queued and is retried
            // next tick, rather than dropping entities nobody has saved.
            LevelEntityStore* entityStore = level.Entities();
            std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> occupied;
            bool occupiedKnown = false;
            std::vector<Game::Math::ChunkPos> deferred;
            while (!level.unloadQueue.empty() &&
                   (mustProcess > 0 || !haveDeadline || std::chrono::steady_clock::now() < m_tickDeadline)) {
                const Game::Math::ChunkPos pos = level.unloadQueue.front();
                level.unloadQueue.pop_front();
                level.unloadQueued.erase(pos);
                if (mustProcess > 0) --mustProcess;
                // Someone may have come back for it while it waited.
                if (wanted(pos)) continue;
                if (entityStore) {
                    if (!occupiedKnown) {
                        occupied = entityStore->OccupiedChunks();
                        occupiedKnown = true;
                    }
                    if (!entityStore->ReadyToUnload(pos, occupied.count(pos) != 0)) {
                        deferred.push_back(pos);
                        continue;
                    }
                }
                {
                    PROFILE_ZONE_N("Unload.CacheRemove");
                    if (!chunkProvider->UnloadChunk(pos)) continue;
                }
                if (auto* light = world->Light()) light->RemoveChunk(pos);
                unloaded++;
                unloadedForMobs.push_back(pos);
            }
            for (const Game::Math::ChunkPos& pos : deferred) {
                if (level.unloadQueued.insert(pos).second) level.unloadQueue.push_back(pos);
            }

            // Entities of this tick's unloads: saved together (one pass over
            // the level's entities, writes on the I/O worker), BEFORE the
            // managers below let go of them.
            if (level.Entities() && !unloadedForMobs.empty()) {
                PROFILE_ZONE_N("Unload.EntitySave");
                level.Entities()->SaveAndForgetMany(unloadedForMobs);
            }
            for (const auto& pos : unloadedForMobs) {
                if (level.Items()) level.Items()->RemoveInChunk(pos, removedItems);
                if (level.Orbs()) level.Orbs()->RemoveInChunk(pos, removedItems);
            }

            if (!unloadedForMobs.empty()) {
                PROFILE_ZONE_N("Unload.Mobs");
                std::vector<EntityPacketOut> outgoing;
                if (level.Mobs()) {
                    std::vector<int32_t> removedMobs;
                    level.Mobs()->RemoveInChunks(unloadedForMobs, removedMobs);
                    if (!removedMobs.empty() && level.MobTracker()) {
                        for (int32_t id : removedMobs) {
                            level.MobTracker()->RemoveEntity(id, outgoing);
                        }
                    }
                }
                if (level.FallingBlocks()) {
                    level.FallingBlocks()->RemoveInChunks(unloadedForMobs, outgoing);
                }
                for (const auto& packet : outgoing) {
                    auto session = m_sessionManager->GetSession(packet.connectionId);
                    if (!session || !session->GetConnection()) continue;
                    session->GetConnection()->SendPacketIn(level.Dimension(),
                        static_cast<uint8_t>(packet.packetId), packet.payload);
                }
            }

            if (!removedItems.empty()) {
                BroadcastItemEntityRemovals(level.Dimension(), removedItems);
            }
        });

        // MC ChunkMap.saveChunksEagerly (called from processUnloads): up to 20
        // dirty chunks a tick, only while the tick has time left, so freshly
        // generated terrain reaches disk steadily instead of in one pile at
        // the next autosave — the pile was the 13 s pause-save.
        if (m_tickDeadline.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() < m_tickDeadline) {
            PROFILE_ZONE_N("SaveChunksEagerly");
            ForEachLevel([&](ServerLevel& level) {
                if (level.World()) level.World()->SaveDirtyChunksEagerly(20, m_tickDeadline);
            });
        }

        // One line per ~3 s, not per tick.
        m_unloadedSinceLog += unloaded;
        if (m_currentServerTick % 60 == 0 && m_unloadedSinceLog > 0) {
            Log::Info("Unloaded %zu unwatched chunks", m_unloadedSinceLog);
            m_unloadedSinceLog = 0;
        }
    }

    void IntegratedServer::ProcessBlockAction(const Network::BlockActionC2SPacket& packet) {
        if (!ValidateBlockAction(packet)) {
            Log::Warning("Invalid block action received");
            return;
        }
        
        switch (packet.action) {
            // Both legacy BREAK and the staged STOP_DESTROY finalize the
            // dig and clear the block. START/ABORT are purely informational
            // (matches PlayerSession::HandleBlockAction).
            case Network::BlockActionType::BREAK:
            case Network::BlockActionType::STOP_DESTROY:
                ApplyBlockChange(packet.worldX, packet.worldY, packet.worldZ, Game::BlockID::Air);
                break;

            case Network::BlockActionType::PLACE:
                ApplyBlockChange(packet.worldX, packet.worldY, packet.worldZ, packet.blockId);
                break;

            case Network::BlockActionType::INTERACT:
                // TODO: Handle block interaction
                Log::Debug("Block interaction at (%d, %d, %d)",
                          packet.worldX, packet.worldY, packet.worldZ);
                break;

            case Network::BlockActionType::START_DESTROY:
            case Network::BlockActionType::ABORT_DESTROY:
                // Informational only (no server-side per-player progress).
                break;
        }
        
        m_stats.blockChangesProcessed.fetch_add(1, std::memory_order_relaxed);
    }

    bool IntegratedServer::ValidateBlockAction(const Network::BlockActionC2SPacket& packet) const {
        // Basic validation
        if (packet.worldY < -64 || packet.worldY > 319) {
            return false;
        }
        
        // Check if position is within reasonable distance from player
        float distance = glm::distance(
            glm::vec3(packet.worldX, packet.worldY, packet.worldZ),
            GetPlayerPosition()
        );
        
        if (distance > 10.0f) { // Max reach distance
            return false;
        }
        
        return true;
    }

    void IntegratedServer::ApplyBlockChange(int worldX, int worldY, int worldZ, Game::BlockID blockId) {
        // OVERWORLD. This is the legacy BlockActionC2S path, reached only from
        // ProcessBlockAction — a packet with no connection id on it, so there
        // is no player and no dimension to resolve from. (The live block path
        // is PlayerSession::HandleBlockAction, which does have a session.)
        Game::World* world = Overworld().World();
        if (!world) {
            return;
        }

        // Apply change to world
        bool success = world->SetBlock(worldX, worldY, worldZ, blockId);
        if (success) {
            // Send block change to client
            Network::BlockChangeS2CPacket packet;
            packet.worldX = worldX;
            packet.worldY = worldY;
            packet.worldZ = worldZ;
            packet.newBlockId = blockId;
            SendPacketToClient(std::move(packet));

#if ENABLE_PORTAL_GUN
            // Remove any portal whose wall blocks were just modified —
            // breaking either of the two wall blocks behind a portal
            // destroys it.
            Game::Portal::ServerRegistry().OnBlockChanged(
                Game::DimensionId::Overworld, glm::ivec3(worldX, worldY, worldZ));
#endif

            Log::Debug("Applied block change at (%d, %d, %d): %d",
                      worldX, worldY, worldZ, static_cast<int>(blockId));
        } else {
            Log::Warning("Failed to apply block change at (%d, %d, %d)", worldX, worldY, worldZ);
        }
    }

    // ========================================================================
    // PLAYER UPDATE PROCESSING
    // ========================================================================

    void IntegratedServer::ProcessChatMessage(const Network::ChatMessageC2SPacket& packet) {
        // Process chat message or command
        if (packet.isCommand) {
            Log::Info("[Command] %s", packet.message.c_str());
            // TODO: Process server commands
        } else {
            Log::Info("[Chat] %s", packet.message.c_str());
            // TODO: Broadcast to other players in multiplayer
        }
        m_stats.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    }
    
    // ── Sleeping ────────────────────────────────────────────────────────

    int IntegratedServer::SleepStatus::SleepersNeeded(int percentage) const {
        // MC SleepStatus.sleepersNeeded: max(1, ceil(active * pct / 100)).
        return std::max(1, static_cast<int>(std::ceil(
            static_cast<float>(activePlayers * percentage) / 100.0f)));
    }

    void IntegratedServer::UpdateSleepingPlayerList() {
        if (!m_sessionManager) return;
        const auto sessions = m_sessionManager->GetAllSessions();

        // MC SleepStatus.update over the Overworld's players: spectators do
        // not count, and only the Overworld has nights to skip (a Nether bed
        // never gets this far).
        const int oldActive   = m_sleepStatus.activePlayers;
        const int oldSleeping = m_sleepStatus.sleepingPlayers;
        int active = 0, sleeping = 0;
        for (const auto& session : sessions) {
            const ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            if (!player || player->getGameMode() == GameMode::SPECTATOR) continue;
            if (Game::DimensionFromRaw(player->getDimensionId()) != Game::DimensionId::Overworld) continue;
            ++active;
            if (player->isSleeping()) ++sleeping;
        }
        m_sleepStatus.activePlayers   = active;
        m_sleepStatus.sleepingPlayers = sleeping;

        const bool changed = (oldSleeping > 0 || sleeping > 0) &&
                             (oldActive != active || oldSleeping != sleeping);
        if (changed) AnnounceSleepStatus();
    }

    void IntegratedServer::AnnounceSleepStatus() {
        // MC ServerLevel.announceSleepStatus. Nothing when no amount of rest
        // can pass the night (canSleepThroughNights), and nothing when you
        // are alone — MC's `!isSingleplayer() || isPublished()`: the count
        // is for the people you share the night with.
        const int percentage = Game::Rules::GetInt(Game::Rules::Id::PlayersSleepingPercentage);
        if (percentage > 100 || !m_sessionManager) return;
        const auto sessions = m_sessionManager->GetAllSessions();
        if (sessions.size() <= 1) return;

        const std::string message = m_sleepStatus.AreEnoughSleeping(percentage)
            ? std::string("Sleeping through this night")
            : std::to_string(m_sleepStatus.sleepingPlayers) + "/" +
              std::to_string(m_sleepStatus.SleepersNeeded(percentage)) + " players sleeping";
        for (const auto& session : sessions) {
            if (session) session->SendOverlayMessage(message);
        }
    }

    void IntegratedServer::TickSleep() {
        if (!m_sessionManager) return;
        UpdateSleepingPlayerList();

        // ServerLevel.tick: enough in bed AND enough of them asleep for the
        // full 100 ticks (SleepStatus.areEnoughDeepSleeping).
        const int percentage = Game::Rules::GetInt(Game::Rules::Id::PlayersSleepingPercentage);
        if (m_sleepStatus.sleepingPlayers <= 0 || !m_sleepStatus.AreEnoughSleeping(percentage)) return;
        int deepSleepers = 0;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            const ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            if (player && player->isSleepingLongEnough()) ++deepSleepers;
        }
        if (deepSleepers < m_sleepStatus.SleepersNeeded(percentage)) return;

        // ClockTimeMarkers.WAKE_UP_FROM_SLEEP — tick 0 of the 24000-tick day,
        // so the clock moves to the NEXT multiple of 24000 (the old
        // `dayTime + 24000 - dayTime % 24000`). Only while the clock runs
        // (advance_time), on every level since each keeps its own copy, and
        // the clients are told at once rather than on the next 20-tick sync.
        if (Game::Rules::GetBool(Game::Rules::Id::AdvanceTime)) {
            ForEachLevel([](ServerLevel& level) {
                Game::World* w = level.World();
                if (!w) return;
                const int64_t t = w->GetDayTime();
                const int64_t into = ((t % 24000) + 24000) % 24000;
                w->SetDayTime(t + 24000 - into);
            });
            ForceTimeSync();
        }
        WakeUpAllPlayers();
        // advance_weather → resetWeatherCycle: there is no weather to clear.
    }

    void IntegratedServer::WakeUpAllPlayers() {
        // MC ServerLevel.wakeUpAllPlayers: the count is dropped first, then
        // every sleeper gets a soft wake (the fade-out plays) without
        // re-running the count per player.
        m_sleepStatus.sleepingPlayers = 0;
        if (!m_sessionManager) return;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            const ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            if (player && player->isSleeping()) session->StopSleepInBed(false, false);
        }
    }

    void IntegratedServer::BroadcastSystemMessage(const std::string& text, uint32_t color) {
        // MC PlayerList.broadcastSystemMessage(message, false) — sender-less,
        // position 0 (chat, not the action bar), everyone gets it.
        if (!m_networkServer) return;
        Network::ChatMessageS2CPacket packet;
        packet.senderId = 0;   // 0 = system message (no chat bubble, no name prefix)
        // Position 1 = system. MC's PlayerList.broadcastSystemMessage sends a
        // ClientboundSystemChatPacket with overlay=false — a SYSTEM message
        // shown in the chat box, not a player chat line and not the action bar.
        // Every command's feedback here already uses 1; join/leave/death belong
        // in the same channel.
        packet.position = 1;
        packet.segments.push_back(Network::ChatSegmentData{
            text, color, Network::ChatClickAction::None, "", ""});

        for (const auto& conn : m_networkServer->GetConnections()) {
            if (conn && conn->IsConnected() && conn->IsAuthenticated()) {
                conn->SendChatMessage(packet);
            }
        }
        Log::Info("[SYSTEM] %s", text.c_str());
    }

    void IntegratedServer::OnPlayerJoined(std::shared_ptr<ServerConnection> connection) {
        Log::Info("[IntegratedServer] Player joined! Setting up session...");

        if (!m_sessionManager) {
            Log::Warning("[IntegratedServer] Session manager not available, cannot handle player join");
            return;
        }

        // Register connection with send scheduler
        if (m_sendScheduler) {
            m_sendScheduler->RegisterConnection(connection->GetConnectionId(), connection);
        }

        // Tell the client what commands exist, for tab-completion. MC sends its
        // whole Brigadier tree here (ClientboundCommandsPacket, from
        // PlayerList.placeNewPlayer); this dispatcher only has names to send.
        //
        // Sent unconditionally on join so a client that joins a server with a
        // different command set — or an older/newer build — completes against
        // that server's commands rather than its own baked-in guess.
        if (m_networkServer) {
            Network::CommandsS2CPacket commandsPacket(m_commandDispatcher.GetCommandNames());
            auto commandsData = Network::Serialization::Serialize(commandsPacket);
            m_networkServer->SendPacketTo(connection->GetConnectionId(),
                static_cast<uint8_t>(Network::PacketId::CommandsS2C), commandsData);
        }

        // MC PlayerList.placeNewPlayer -> tickRateManager().updateJoiningPlayer.
        // A client joining an already-frozen world has to learn about it here,
        // or its mobs keep animating until someone toggles the freeze.
        m_tickRateManager.updateJoiningPlayer(*connection);

        // Use connection ID as unique player ID, player name from login packet
        uint32_t playerId = connection->GetConnectionId();
        std::string requestedName = connection->GetPlayerName();

        // Helper: is this name already taken by an active session?
        auto isNameTaken = [&](const std::string& candidate) {
            for (const auto& session : m_sessionManager->GetAllSessions()) {
                if (session && session->GetPlayer() &&
                    session->GetPlayer()->getName() == candidate) {
                    return true;
                }
            }
            return false;
        };

        // Default name = kDefaultPlayerName ("Notch"), and only numbered if
        // that is actually taken by someone else currently online.
        //
        // It used to be "Player" + connectionId, which is unique but not
        // STABLE: every rejoin within a session produced Player1, Player2,
        // Player3... That is visible to the player, and it is now load-bearing
        // — playerdata/<uuid>.dat derives its UUID from
        // "OfflinePlayer:" + name, so a name that changes each join means a
        // fresh, empty player file each join and an inventory that never comes
        // back. Uniqueness among concurrent players still holds; it just is
        // not bought with a number that changes every time.
        std::string defaultName = Server::kDefaultPlayerName;
        for (int suffix = 2; isNameTaken(defaultName) && suffix < 1000; ++suffix) {
            defaultName = std::string(Server::kDefaultPlayerName) + std::to_string(suffix);
        }

        // Resolution rules (per user spec):
        //   - Empty requested name → use default
        //   - Requested name not taken → use it
        //   - Requested name collides with an existing player → fall back to default
        //     (instead of suffix-mangling, so "Bob" trying to join while "Bob" is here
        //     becomes "Player2" rather than "Bob_2")
        std::string playerName;
        if (requestedName.empty() || isNameTaken(requestedName)) {
            playerName = defaultName;
        } else {
            playerName = requestedName;
        }

        // Update connection so chat (<name> message) and disconnect logs use the resolved name
        connection->SetPlayerName(playerName);

        // MC IntegratedServer.java:269-270 — a case-insensitive NAME compare
        // against the host's profile decides isSingleplayerOwner, and the owner
        // is exempt from keep-alive and the read timeout entirely.
        //
        // Compare the RESOLVED name, never the requested one: the collision
        // policy just above already renamed any impostor, which is our
        // equivalent of MC's name_taken rejection.
        //
        // NOT keyed on IsLoopback(), and that is deliberate — a friend joining
        // through the relay hands the host a socket dialled to the friends
        // service, which CLAUDE.md documents as 127.0.0.1 on the hosting
        // machine, so a remote WAN player really can present a loopback
        // endpoint. See the note on NetworkConnection::IsLoopback.
        //
        // The compare_exchange makes it first-claimant-wins, so even a spoofed
        // --name can only ever cost the exemption for one connection.
        const std::string ownerName = m_config.singleplayerProfileName.empty()
                                          ? defaultName
                                          : m_config.singleplayerProfileName;
        if (m_config.hasSingleplayerOwner &&
            EqualsIgnoreCaseAscii(playerName, ownerName)) {
            uint32_t expected = 0;
            if (m_singleplayerOwnerConnId.compare_exchange_strong(
                    expected, connection->GetConnectionId())) {
                connection->SetSingleplayerOwner(true);
                Log::Info("[IntegratedServer] Connection %u is the singleplayer owner ('%s') — "
                          "exempt from keep-alive and read timeout (MC IntegratedServer.java:269)",
                          connection->GetConnectionId(), playerName.c_str());
            }
        }

        // Determine which ServerPlayer to use:
        // - Connection 1 (host): use existing m_serverPlayer
        // - Other connections: create a new ServerPlayer
        ServerPlayer* playerPtr = nullptr;
        if (playerId == 1 && m_serverPlayer) {
            playerPtr = m_serverPlayer.get();
            // Host's m_serverPlayer was constructed with kDefaultPlayerName before we
            // knew the resolved name. Sync it now so /tp <name> and the PlayerInfo broadcast
            // both see the same name as chat does.
            playerPtr->setName(playerName);
        } else {
            auto remotePlayer = std::make_unique<ServerPlayer>(playerId, playerName);
            remotePlayer->setPosition(glm::dvec3(m_worldSpawn));
            playerPtr = remotePlayer.get();
            m_remotePlayers[playerId] = std::move(remotePlayer);
            Log::Info("[IntegratedServer] Created ServerPlayer for remote player '%s' (ID: %u)",
                      playerName.c_str(), playerId);
        }
        // Restore this player from disk, if they have been here before.
        //
        // Must happen AFTER the name is resolved: the file is named by a UUID
        // derived from the name, so loading earlier would look for the wrong
        // file. A first-time player simply has none, which is not an error.
        const bool restoredFromDisk = LoadPlayerData(*playerPtr);
        // A first-time player faces the way the world's spawn says (MC
        // PlayerList.placeNewPlayer reads the level's spawn angle); a returning
        // one keeps the look direction ReadPlayerData restored.
        if (!restoredFromDisk && m_savedLevelDat && m_savedLevelDat->hasSpawn) {
            playerPtr->setRotation(m_savedLevelDat->spawnYaw, m_savedLevelDat->spawnPitch);
        }

        // Capture the colour the client sent at LoginStart onto the ServerPlayer so
        // both the new-player broadcast (below) and any future PlayerInfo refreshes
        // pull from one canonical source.
        playerPtr->setColorId(connection->GetPlayerColor());

        // PlayerInfo: send all existing players to the new client BEFORE adding it
        // (matching MC's PlayerList.placeNewPlayer line 185 — connection.send(createPlayerInitializing(this.players)))
        for (const auto& existing : m_sessionManager->GetAllSessions()) {
            if (existing && existing->GetPlayer()) {
                Network::PlayerInfoS2CPacket addExisting;
                addExisting.action = Network::PlayerInfoS2CPacket::Action::ADD;
                addExisting.playerId = existing->GetPlayerId();
                addExisting.playerName = existing->GetPlayer()->getName();
                addExisting.colorId = existing->GetPlayer()->getColorId();
                auto data = Network::Serialization::Serialize(addExisting);
                connection->SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerInfoS2C), data);
            }
        }

        // Create session via SessionManager
        m_sessionManager->OnPlayerJoin(playerId, connection->GetConnectionId(), playerName);

        // Attach ServerPlayer to the session
        auto session = m_sessionManager->GetSession(playerId);
        if (session) {
            session->AttachPlayer(playerPtr);
            session->SetConnection(connection.get());

            // Apply the client settings that arrived while this session was
            // being built — by now it is Initialize()d, so the value sticks.
            // Doing it here rather than waiting for the tick means the FIRST
            // batch of chunks already goes out at the right radius. The tick
            // repeats the call as a safety net for a stash that landed after
            // this point. See OnClientSettingsReceived.
            ApplyPendingClientViewDistances();

            Log::Info("[IntegratedServer] Player '%s' (ID: %u) session created and wired to connection %u",
                      playerName.c_str(), playerId, connection->GetConnectionId());

            // ── Switch to PLAY *before* any join packet goes out ────────────
            //
            // MC PlayerList.placeNewPlayer:153-154 constructs the game listener
            // and calls setupInboundProtocol as its first act, and does not
            // reach connection.teleport until :179. The order matters: the join
            // teleport provokes an immediate ack from the client, and anything
            // that arrives before the connection reads as PLAY is answering a
            // question we have not finished asking.
            //
            // This used to happen at the END of finalizeLogin, after the
            // teleport below. On Windows the ack won that race, the phase check
            // in the old teleport-ack handler discarded it, and the player's
            // movement was gated off for the rest of the session.
            connection->setProtocolState(Network::ProtocolState::PLAY, session.get());

            // Apply the world's game mode and sync abilities. Mirrors MC
            // PlayerList.placeNewPlayer applying the level's default game
            // type to every joiner. The login-time packet
            // (SendPlayerAbilitiesForJoin) already carried this same mode, so
            // this is a reconfirmation against the live ServerPlayer rather
            // than a correction — the client is never told "survival" first.
            // FIRST JOIN ONLY. A returning player's mode came from their save
            // in LoadPlayerData, and setGameMode also clears m_flying — so
            // running it unconditionally is what used to land every returning
            // creative flier back in survival, falling.
            // MC IntegratedServer.forceGameMode (default on): a GUEST is put
            // in the world's default game mode every join, saved mode or not;
            // the host keeps theirs.
            const bool forcedGuest = m_forceGameMode.load() && !connection->IsSingleplayerOwner();
            if (!restoredFromDisk || forcedGuest) {
                playerPtr->setGameMode(static_cast<GameMode>(m_config.defaultGameMode));
            }
            connection->SendPlayerAbilities(*playerPtr);

            // Send full 46-slot inventory snapshot. Replaces the old HotbarSyncS2C path —
            // InventoryFullS2C carries real per-slot counts so the client doesn't have to
            // synthesize them.
            session->SendInventoryFull();
            Log::Info("[IntegratedServer] Sent full inventory sync to client");

            // A returning player who logged out in another dimension: the
            // client boots assuming the Overworld, so before the join
            // teleport it must be told the real dimension (sky, build
            // range), and the LEVEL must exist for the chunk stream —
            // LevelOf(session) resolves whatever the player's saved
            // dimension says. MC does this through the login packet's
            // dimension field; this engine's equivalent is the same
            // ChangeDimensionS2C a portal crossing sends. Fresh joiners are
            // dimension 0 and skip all of it.
            if (playerPtr->getDimensionId() != 0) {
                const Game::DimensionId dim =
                    Game::DimensionFromRaw(playerPtr->getDimensionId());
                GetOrCreateLevel(dim);

                const auto dimPacket =
                    Network::ChangeDimensionS2CPacket::For(dim, /*keepPrevious=*/false,
                                                           GetBiomeZoomSeed());
                connection->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::ChangeDimensionS2C),
                    Network::Serialization::Serialize(dimPacket));
                connection->SetOutboundDimension(dim);
                Log::Info("[IntegratedServer] '%s' rejoins in dimension %d",
                          playerName.c_str(), playerPtr->getDimensionId());
            }

            // MC PlayerList.placeNewPlayer:171-178 —
            //
            //   component = Component.translatable("multiplayer.player.joined",
            //                                      player.getDisplayName());
            //   this.broadcastSystemMessage(component.withStyle(ChatFormatting.YELLOW), false);
            //
            // and it sits right here, immediately before connection.teleport
            // (:179). ChatFormatting.YELLOW is 0xFFFF55.
            //
            // The `.renamed` variant ("%s (formerly known as %s) joined the
            // game") is deliberately not ported: it fires when the profile
            // cache holds a different previous name for the same UUID, and we
            // have no such cache — names come from the launcher each session.
            BroadcastSystemMessage(playerName + " joined the game", 0xFFFFFF55u);

            // Teleport the new client to the player's server-side position
            // (world spawn for fresh players). The client boots at its own
            // hardcoded coords; without this, it stays there no matter what
            // spawn the server picked. Mirrors MC PlayerList.placeNewPlayer's
            // connection.teleport(...) on login.
            {
                const glm::dvec3 pos = playerPtr->getPosition();
                // The player's OWN rotation, not (0,0). ReadPlayerData
                // restores it onto the ServerPlayer and this teleport is what
                // the client actually obeys, so hardcoding zero here threw the
                // saved look direction away on every single join.
                connection->Teleport(pos.x, pos.y, pos.z,
                                     playerPtr->getYaw(), playerPtr->getPitch());
                Log::Info("[IntegratedServer] Teleported '%s' to spawn (%.1f, %.1f, %.1f)",
                          playerName.c_str(), pos.x, pos.y, pos.z);
            }

            // PlayerInfo: broadcast new player to ALL clients (including the new one, so they see
            // themselves in the player list — matching MC's PlayerList.placeNewPlayer line 188)
            if (m_networkServer) {
                Network::PlayerInfoS2CPacket addNew;
                addNew.action = Network::PlayerInfoS2CPacket::Action::ADD;
                addNew.playerId = playerId;
                addNew.playerName = playerName;
                addNew.colorId = playerPtr->getColorId();
                auto data = Network::Serialization::Serialize(addNew);

                for (const auto& conn : m_networkServer->GetConnections()) {
                    if (conn && conn->IsConnected()) {
                        conn->SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerInfoS2C), data);
                    }
                }
                Log::Info("[IntegratedServer] Broadcast PlayerInfo ADD for '%s' (ID: %u)",
                          playerName.c_str(), playerId);
            }

#if ENABLE_PORTAL_GUN
            // Catch the new client up to every currently active portal so
            // they render immediately rather than waiting for someone to
            // re-fire. No-op when no portals exist.
            Game::Portal::ServerRegistry().SyncToClient(connection.get());
#endif
        } else {
            Log::Error("[IntegratedServer] Failed to retrieve session after OnPlayerJoin for player %u", playerId);
        }
    }

    // ========================================================================
    // PLAYER DISCONNECT
    // ========================================================================

    void IntegratedServer::DrainClosedConnections() {
        std::vector<std::shared_ptr<ServerConnection>> closed;
        {
            std::lock_guard<std::mutex> lock(m_closedMutex);
            closed.swap(m_closedConnections);
        }
        for (auto& conn : closed) {
            if (conn) OnPlayerDisconnected(conn);
        }
    }

    void IntegratedServer::OnPlayerDisconnected(std::shared_ptr<ServerConnection> connection) {
        uint32_t connectionId = connection->GetConnectionId();
        // Free the singleplayer-owner slot if this was the holder, so a
        // reconnecting host can claim it again.
        {
            uint32_t owner = connectionId;
            m_singleplayerOwnerConnId.compare_exchange_strong(owner, 0u);
        }
        uint32_t playerId = connection->GetPlayerId();
        std::string playerName = connection->GetPlayerName();

        // Save on the way out. This is where MC does it too (PlayerList.remove
        // -> playerIo.save), and it is the only place that works: by the time
        // IntegratedServer::Shutdown runs, Stop() has already torn down the
        // session manager and with it every ServerPlayer, inventory included.
        // Same lookup the rest of this class uses: connection id 1 is the
        // host's ServerPlayer, everyone else lives in m_remotePlayers.
        {
            const ServerPlayer* leaving = nullptr;
            if (connectionId == 1 && m_serverPlayer) {
                leaving = m_serverPlayer.get();
            } else {
                auto it = m_remotePlayers.find(connectionId);
                if (it != m_remotePlayers.end()) leaving = it->second.get();
            }
            if (leaving) SavePlayerData(*leaving);
        }

        Log::Info("[IntegratedServer] Player '%s' (ID: %u, conn: %u) disconnected",
                  playerName.c_str(), playerId, connectionId);

        // MC ServerGamePacketListenerImpl.removePlayerFromWorld:1388 —
        //
        //   this.server.getPlayerList().broadcastSystemMessage(
        //       Component.translatable("multiplayer.player.left",
        //                              this.player.getDisplayName())
        //           .withStyle(ChatFormatting.YELLOW), false);
        //
        // Covers every exit for the same reason it does in MC: quitting, a
        // dropped connection and a kick all funnel through the same teardown,
        // so vanilla has no separate "was kicked" broadcast — the kicked player
        // sees the reason on their disconnect screen, everyone else just sees
        // them leave.
        //
        // Only for players who actually made it into the world: a connection
        // that died during handshake/login has no name to announce.
        if (!playerName.empty() && connection->IsAuthenticated()) {
            BroadcastSystemMessage(playerName + " left the game", 0xFFFFFF55u);
        }

        // Drop any client settings still waiting on a session that will now
        // never exist — a reused connection id must not inherit them.
        {
            std::lock_guard<std::mutex> lock(m_pendingViewDistanceMutex);
            m_pendingClientViewDistance.erase(connectionId);
        }

        // Forget everything this player was tracking, in EVERY level. Connection
        // ids ARE reused, so a leftover watch set would make the next player to
        // take this id silently never receive spawn packets for those mobs —
        // and a player who used a portal is tracked in both worlds, so clearing
        // only the one they happened to be standing in would leave the other.
        ForEachLevel([connectionId](ServerLevel& level) {
            if (level.MobTracker()) level.MobTracker()->RemovePlayer(connectionId);
            if (level.FallingBlocks()) level.FallingBlocks()->RemovePlayer(connectionId);
        });

        // 1. Broadcast RemoveEntities to all remaining clients
        if (m_networkServer && playerId != 0) {
            Network::RemoveEntitiesS2CPacket removePacket(static_cast<int32_t>(playerId));
            auto data = Network::Serialization::Serialize(removePacket);

            auto connections = m_networkServer->GetConnections();
            for (const auto& conn : connections) {
                if (conn->GetConnectionId() != connectionId && conn->IsConnected()) {
                    conn->SendPacket(
                        static_cast<uint8_t>(Network::PacketId::EntityDestroy), data);
                }
            }
            Log::Info("[IntegratedServer] Broadcast RemoveEntities (ID: %u) to %zu clients",
                      playerId, connections.size());

            // PlayerInfo: broadcast REMOVE to all remaining clients
            // (matching MC's PlayerList.remove line 308 — broadcastAll(new ClientboundPlayerInfoRemovePacket(...)))
            Network::PlayerInfoS2CPacket removeInfo;
            removeInfo.action = Network::PlayerInfoS2CPacket::Action::REMOVE;
            removeInfo.playerId = playerId;
            auto removeData = Network::Serialization::Serialize(removeInfo);

            for (const auto& conn : connections) {
                if (conn->GetConnectionId() != connectionId && conn->IsConnected()) {
                    conn->SendPacket(
                        static_cast<uint8_t>(Network::PacketId::PlayerInfoS2C), removeData);
                }
            }
            Log::Info("[IntegratedServer] Broadcast PlayerInfo REMOVE for player %u", playerId);
        }

        // 2. Unregister from SendScheduler
        if (m_sendScheduler) {
            m_sendScheduler->UnregisterConnection(connectionId);
        }

        // 3. Clean up session (watch index, chunk state, etc.)
        if (m_sessionManager) {
            m_sessionManager->OnPlayerLeave(playerId, "Disconnected");
        }

        // 4. Retire this player's PlayerEntityView in every level, BEFORE the
        //    ServerPlayer it points at is freed.
        //
        // This ordering is load-bearing and it used to be wrong: the erase
        // below ran FIRST, so for the rest of the teardown every level still
        // held a PlayerEntityView whose ServerPlayer* was dangling, and the
        // session still handed that same pointer out of GetPlayer(). One more
        // server tick in that window and a mob's TemptGoal called
        // ServerLevelBridge::GetHeldItemId on freed memory — a hard SIGSEGV on
        // "Save and Quit", with a stack of
        // MobManager::Tick -> Mob::ServerAiStep -> TemptGoal::CanUse.
        //
        // SyncPlayerViews is what drops a view, and it does the necessary
        // ClearReferenceTo sweep over every mob first. It only drops views
        // whose session is gone, which is why step 3 has to precede it.
        ForEachLevel([](ServerLevel& level) {
            if (ServerLevelBridge* bridge = level.MobLevel()) bridge->SyncPlayerViews();
        });

        // 5. NOW the ServerPlayer can go. Nothing points at it any more.
        m_remotePlayers.erase(playerId);
    }

    // ========================================================================
    // CLIENT SETTINGS
    // ========================================================================

    void IntegratedServer::OnClientSettingsReceived(uint32_t connectionId, int requestedViewDistance,
                                                    int requestedSimulationDistance) {
        // Note the ordering: a missing session manager must still stash, not
        // return. Bailing out before the stash would lose the settings exactly
        // when they are most likely to arrive early — during startup.
        // Runs on the NETWORK I/O thread. It only stashes; the server thread
        // applies (ApplyPendingClientViewDistances). See the header for why
        // both halves of that matter.
        //
        // The bug this replaces is worth spelling out, because "the session
        // does not exist yet" was only half of it. A session EXISTS from
        // PlayerSessionManager::CreateSession, several statements before
        // OnPlayerJoin gets round to Initialize() — and Initialize assigns both
        // distances from its Config. So the usual sequence was:
        //
        //   Created session for player 1
        //   Player 1 requested view distance 16, effective: 16
        //   Player 1 view distance changed to 8      <- clamped by the DEFAULT
        //                                              m_simulationDistance
        //   Sent SetChunkCacheRadius(16)             <- client believes 16
        //   Initialized session for player 1         <- resets it to 2
        //   UpdateChunkTracking: ... viewDist=2
        //
        // Applied, acknowledged to the client, then silently overwritten. The
        // client rendered out to 16 and the server never sent more than the 25
        // chunks a view distance of 2 covers.
        {
            std::lock_guard<std::mutex> lock(m_pendingViewDistanceMutex);
            m_pendingClientViewDistance[connectionId] =
                ClientDistances{ requestedViewDistance, requestedSimulationDistance };
        }
        Log::Info("[IntegratedServer] Client settings for connection %u: view distance %d, "
                  "simulation distance %d queued for the server thread",
                  connectionId, requestedViewDistance, requestedSimulationDistance);
    }

    void IntegratedServer::ApplyPendingClientViewDistances() {
        if (!m_sessionManager) return;

        // Swap the map out rather than holding the lock across the session
        // lookups below — those take PlayerSessionManager's own mutex, and
        // nesting two locks in one order here and the other order anywhere
        // else is how this deadlocks later.
        std::unordered_map<uint32_t, ClientDistances> pending;
        {
            std::lock_guard<std::mutex> lock(m_pendingViewDistanceMutex);
            if (m_pendingClientViewDistance.empty()) return;
            pending.swap(m_pendingClientViewDistance);
        }

        std::unordered_map<uint32_t, ClientDistances> stillWaiting;
        for (const auto& [connectionId, requested] : pending) {
            auto session = m_sessionManager->GetSessionByConnection(connectionId);
            // Not ready is not a failure: the join is mid-flight and the next
            // tick will find it. Only a DISCONNECT drops the entry, which
            // OnPlayerDisconnected does explicitly.
            if (!session || !session->IsInitialized()) {
                stillWaiting[connectionId] = requested;
                continue;
            }
            ApplyClientViewDistance(*session, connectionId, requested);
        }

        if (!stillWaiting.empty()) {
            std::lock_guard<std::mutex> lock(m_pendingViewDistanceMutex);
            // Merge, don't assign: a newer value may have arrived from the I/O
            // thread while this ran, and it must win over the one we are
            // putting back.
            for (const auto& [connectionId, requested] : stillWaiting) {
                m_pendingClientViewDistance.emplace(connectionId, requested);
            }
        }
    }

    void IntegratedServer::ApplyClientViewDistance(PlayerSession& session,
                                                   uint32_t connectionId,
                                                   ClientDistances requested) {
        // Clamp client's requested distance to [2, serverViewDistance]
        const int effectiveViewDistance =
            std::clamp(requested.viewDistance, 2, m_config.serverViewDistance);
        // MC IntegratedServer.tickServer: max(2, options.simulationDistance).
        // MC caps it by the view distance because its chunk-level scale ends
        // at 32; here the scale has headroom (ChunkLevel::SIMULATION_HEADROOM)
        // and the ring beyond the view distance is loaded by a Simulation
        // loader (ComputeChunkLoaders), so the cap is its own number.
        const int effectiveSimulationDistance =
            std::clamp(requested.simulationDistance, 2,
                       std::min(m_config.maxSimulationDistance, ChunkLevel::kMaxSimulationDistance));

        Log::Info("[IntegratedServer] Player %u requested view distance %d (effective %d), "
                  "simulation distance %d (effective %d), server cap %d",
                  session.GetPlayerId(), requested.viewDistance, effectiveViewDistance,
                  requested.simulationDistance, effectiveSimulationDistance,
                  m_config.serverViewDistance);

        // Update session view distance (triggers watch set recalculation)
        session.SetViewDistance(effectiveViewDistance);
        // The ticket manager picks this up on the session's next tick
        // (PlayerSessionManager::UpdatePlayerTickets) and re-levels the
        // player's PLAYER_SIMULATION ticket in place.
        session.SetSimulationDistance(effectiveSimulationDistance);

        // The chunk cache must hold the whole simulation ring plus the view
        // and a stale area, or the LRU evicts ring chunks as fast as they
        // load. Sized per player currently in the dimension (they may stand
        // apart), grow-only (ChunkProvider::SetMaxLoadedChunks).
        {
            const size_t ringSide = 2 * static_cast<size_t>(std::max(effectiveSimulationDistance, effectiveViewDistance) + 2) + 1;
            size_t players = 0;
            if (m_sessionManager) {
                for (const auto& s : m_sessionManager->GetAllSessions()) {
                    if (s && s->GetDimensionId() == session.GetDimensionId()) ++players;
                }
            }
            const size_t needed = ringSide * ringSide * std::max<size_t>(players, 1) + 8192;
            ServerLevel& level = LevelOf(session);
            if (level.World() && level.World()->GetChunkProvider()) {
                level.World()->GetChunkProvider()->SetMaxLoadedChunks(needed);
            }
        }

        // Send effective view distance back to client
        SendSetChunkCacheRadius(connectionId, effectiveViewDistance);
    }

    void IntegratedServer::SendSetChunkCacheRadius(uint32_t connectionId, int viewDistance) {
        if (!m_networkServer || m_shouldStop.load()) return;

        Network::SetChunkCacheRadiusS2CPacket packet(viewDistance);
        auto data = Network::Serialization::Serialize(packet);
        m_networkServer->SendPacketTo(connectionId,
            static_cast<uint8_t>(Network::PacketId::SetChunkCacheRadiusS2C), data);

        Log::Info("[IntegratedServer] Sent SetChunkCacheRadius(%d) to connection %u", viewDistance, connectionId);
    }

    // ========================================================================
    // PACKET SENDING
    // ========================================================================

    void IntegratedServer::SendPacketToClient(Network::BlockChangeS2CPacket&& packet) {
        // Check both that NetworkServer exists and we're not shutting down
        if (m_networkServer && !m_shouldStop.load()) {
            auto data = Network::Serialization::Serialize(packet);
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::BlockChangeS2C), data);
            m_stats.packetsSent.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void IntegratedServer::SendPacketToClient(Network::MultiBlockChangeS2CPacket&& packet) {
        // Check both that NetworkServer exists and we're not shutting down
        if (m_networkServer && !m_shouldStop.load()) {
            auto data = Network::Serialization::Serialize(packet);
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::MultiBlockChangeS2C), data);
            m_stats.packetsSent.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ========================================================================
    // UTILITY METHODS
    // ========================================================================

    float IntegratedServer::CalculateChunkDistance(Game::Math::ChunkPos chunkPos) const {
        // Calculate Chebyshev distance (square pattern like Minecraft)
        // This creates a square loading pattern instead of circular
        auto pos = GetPlayerPosition();
        auto playerChunk = Game::Math::WorldCoordinates::WorldToChunkPos(
            static_cast<int>(pos.x),
            static_cast<int>(pos.z)
        );
        
        int dx = std::abs(chunkPos.x - playerChunk.x);
        int dz = std::abs(chunkPos.z - playerChunk.z);
        
        // Chebyshev distance = max(abs(dx), abs(dz))
        // Convert to float for compatibility with existing code
        return static_cast<float>(std::max(dx, dz));
    }

    std::vector<Game::Math::ChunkPos> IntegratedServer::GetRequiredChunks() const {
        std::vector<Game::Math::ChunkPos> chunks;

        auto playerChunk = GetPlayerChunkPosition();
        int renderDistance = m_config.serverViewDistance;
        
        // Generate square pattern around player
        for (int dx = -renderDistance; dx <= renderDistance; ++dx) {
            for (int dz = -renderDistance; dz <= renderDistance; ++dz) {
                Game::Math::ChunkPos chunkPos{playerChunk.x + dx, playerChunk.z + dz};
                chunks.push_back(chunkPos);
            }
        }
        
        // Sort by distance for priority loading
        std::sort(chunks.begin(), chunks.end(), [this](const Game::Math::ChunkPos& a, const Game::Math::ChunkPos& b) {
            return CalculateChunkDistance(a) < CalculateChunkDistance(b);
        });
        
        return chunks;
    }
    
    void IntegratedServer::UpdateViewDistanceWatchers() {
        // View distance watching is now handled by PlayerSession's watch set system
    }

    void IntegratedServer::UpdateStatistics(float tickExecutionTime, float timeBetweenTicks) {
        // Update average tick execution time (simple moving average)
        float currentAvg = m_stats.averageTickTime.load();
        float newAvg = currentAvg * 0.9f + tickExecutionTime * 0.1f;
        m_stats.averageTickTime.store(newAvg);
        
        // Update TPS based on actual time between tick starts
        // This gives the true tick rate, not the theoretical rate
        if (timeBetweenTicks > 0.0f) {
            float actualTPS = 1000.0f / timeBetweenTicks; // timeBetweenTicks is in milliseconds
            
            // Apply moving average
            float currentTPS = m_stats.averageTPS.load();
            float newTPS = currentTPS * 0.9f + actualTPS * 0.1f;
            m_stats.averageTPS.store(newTPS);
        }
    }

    void IntegratedServer::LogServerState() const {
        auto session = GetPlayerSession();
        size_t sentChunks = session ? session->GetSentChunkCount() : 0;
        // PendingLoads is the sum across every dimension — one number, because
        // the streaming health this line reports is a property of the pool, not
        // of any one world.
        Log::Info("Server State: TPS=%.1f, TickTime=%.2fms, SentChunks=%zu, PendingLoads=%zu",
                 m_stats.averageTPS.load(), m_stats.averageTickTime.load(),
                 sentChunks, GetPendingChunkLoadCount());
        // The generation pipeline, per level: what is waiting for the library,
        // what the library holds, and the counters that tell a stall apart
        // from a slow stream (refused = the library answered "no" at request
        // time; empty = it completed without a chunk; retried = failed loads
        // re-requested).
        for (const auto& levelPtr : m_levels) {
            if (!levelPtr) continue;
            const ServerLevel& level = *levelPtr;
            if (level.generationBacklog.empty() && level.generationInFlight == 0 && level.genCompleted == 0) continue;
            Log::Info("  Generation[%s]: backlog=%zu inFlight=%zu pending=%zu failed=%zu | completed=%llu readyTaken=%llu empty=%llu refused=%llu retried=%llu stuck=%llu | poolJobs=%zu",
                      std::string(Game::DimensionName(level.Dimension())).c_str(),
                      level.generationBacklog.size(), level.generationInFlight,
                      level.pendingChunkLoads.size(), level.failedChunkLoads.size(),
                      static_cast<unsigned long long>(level.genCompleted),
                      static_cast<unsigned long long>(level.genDeliveredReady),
                      static_cast<unsigned long long>(level.genCompletedEmpty),
                      static_cast<unsigned long long>(level.genRefused),
                      static_cast<unsigned long long>(level.genRetried),
                      static_cast<unsigned long long>(level.genStuck),
                      Threading::g_serverWorkerPool ? Threading::g_serverWorkerPool->GetPendingJobCount() : size_t{0});
        }
    }

    // ========================================================================
    // GLOBAL FUNCTIONS
    // ========================================================================

    void InitializeIntegratedServer(const IntegratedServerConfig& config) {
        if (g_integratedServer) {
            Log::Warning("IntegratedServer already initialized");
            return;
        }

        g_integratedServer = std::make_unique<IntegratedServer>(config);
        g_integratedServer->Initialize();
    }

    bool StartIntegratedServer() {
        if (!g_integratedServer) {
            Log::Error("StartIntegratedServer: no integrated server instance");
            return false;
        }
        return g_integratedServer->Start();
    }

    void StopIntegratedServer() {
        if (g_integratedServer) {
            g_integratedServer->Stop();
        }
    }

    void ShutdownIntegratedServer() {
        if (g_integratedServer) {
            g_integratedServer->Shutdown();
            g_integratedServer.reset();
        }
    }

    bool IsIntegratedServerRunning() {
        return g_integratedServer && g_integratedServer->IsRunning();
    }

    const IntegratedServer::ServerStats& GetIntegratedServerStats() {
        static IntegratedServer::ServerStats emptyStats;
        return g_integratedServer ? g_integratedServer->GetStats() : emptyStats;
    }

    // ========================================================================
    // SESSION SYSTEM IMPLEMENTATION
    // ========================================================================

    void IntegratedServer::InitializeSessionSystem() {
        Log::Info("Initializing session management system...");

        // Only the genuinely GLOBAL pieces are built here. The ticket and
        // status managers, the change accumulator and broadcaster, the item,
        // XP and mob systems all belong to a ServerLevel now — and this runs
        // BEFORE any level exists, because a level needs the session manager to
        // construct its mob bridge.
        m_sendScheduler = std::make_unique<SendScheduler>();
        m_sessionManager = std::make_unique<PlayerSessionManager>();
        m_remoteControl  = std::make_unique<RemoteControlManager>(*m_sessionManager);
        m_morphCarry     = std::make_unique<MorphCarry>(*m_sessionManager, *m_remoteControl);
        m_morphBlockAnchor = std::make_unique<MorphBlockAnchor>(*m_sessionManager);
        // Every Level.playSound on this server now reaches its players.
        m_soundBroadcaster = std::make_unique<ServerSoundBroadcaster>(m_sessionManager.get());
        Game::Sound::SetServerSink(m_soundBroadcaster.get());

        // The pathfinder's block classification is derived from the block
        // registry, so it has to be (re)built after BlockRegistry::Init and
        // before any mob paths. Cheap and idempotent.
        Game::InitPathTypeTable();
        // Same contract: derived from the block registry, so it must be built
        // after BlockRegistry::Init and before any chunk is primed.
        Game::InitHeightmapTable();


        // Configure send scheduler
        SendScheduler::Config schedulerConfig;
        schedulerConfig.defaultMaxOutboxBytes = 4194304;  // 4MB
        schedulerConfig.globalMaxBytesPerTick = 10485760;  // 10MB
        schedulerConfig.enableCompression = true;
        m_sendScheduler->Initialize(schedulerConfig);
        
        // Configure session manager
        PlayerSessionManager::Config sessionConfig;
        // Only in force between CreateSession and the client's first
        // ClientConfigC2S a tick later. MC's server.properties default.
        sessionConfig.defaultSimulationDistance = 10;
        sessionConfig.defaultViewDistance = m_config.defaultViewDistance;
        sessionConfig.maxViewDistance = m_config.serverViewDistance;  // Server's view distance cap
        // Placeholder at init time — the server thread recomputes the real
        // spawn at startup and pushes it via SetWorldSpawn (see Start()).
        sessionConfig.worldSpawn = m_worldSpawn;
        sessionConfig.spawnChunkRadius = 2;
        sessionConfig.maxChunksPerPlayerPerTick = m_config.maxChunksPerTick;
        sessionConfig.kickOnTimeout = false;  // Integrated server: never kick local player

        // Nulls for the ticket and status managers: they belong to the
        // overworld, which does not exist yet. Initialize() hands them over via
        // SetLevelServices — together with the spawn tickets that depend on
        // them — the moment the overworld is built.
        m_sessionManager->Initialize(
            sessionConfig,
            nullptr,
            nullptr,
            m_sendScheduler.get()
        );

        Log::Info("Session management system initialized successfully");
    }

    void IntegratedServer::CleanupSessionSystem() {
        Log::Info("Cleaning up session management system...");

        // Only the global pieces. Everything that moved into ServerLevel is
        // torn down by ~ServerLevel, in reverse construction order, when
        // Shutdown() resets the array — which happens AFTER this, so a session
        // being cleaned up here can still reach its level.
        //
        // The sound sink goes first: it walks the session manager, and a
        // level torn down after this may still play a sound on its way out.
        if (m_soundBroadcaster) {
            if (Game::Sound::GetServerSink() == m_soundBroadcaster.get()) {
                Game::Sound::SetServerSink(nullptr);
            }
            m_soundBroadcaster.reset();
        }
        // Emptied, NOT destroyed. Every ServerLevel holds this manager as a
        // raw pointer (the mob bridge, the delta broadcaster, the Silent
        // Warden's bars, the Hush's stillness, Aurelith's cities), and their
        // teardown in ~ServerLevel still asks it for sessions — to send the
        // boss-bar remove, the stillness lift, the city forget. Destroying it
        // here left those pointers dangling until Shutdown() reset the levels:
        // a quit after meeting an Aurelith city locked the freed manager's
        // mutex and aborted ("mutex lock failed: Invalid argument"). Emptied,
        // it answers "no sessions" and those sends are no-ops. MC's PlayerList
        // likewise outlives the levels it serves. The object itself goes with
        // the server, after its levels (see ~IntegratedServer).
        if (m_sessionManager) {
            m_sessionManager->Shutdown();
        }

        if (m_sendScheduler) {
            m_sendScheduler->Shutdown();
            m_sendScheduler.reset();
        }

        Log::Info("Session management system cleaned up");
    }

    void IntegratedServer::SendBlockChangeS2CPacket(Game::DimensionId dimension,
                                                    const Network::BlockChangeS2CPacket& packet) {
        // Check both that NetworkServer exists and we're not shutting down
        if (m_networkServer && !m_shouldStop.load()) {
            auto data = Network::Serialization::Serialize(packet);
            // Scoped to the watchers of this chunk IN this dimension, not
            // broadcast: the packet carries only x/y/z, so every client that
            // received it would apply the edit to its own world at those
            // coordinates — a Nether edit repainting the Overworld.
            SendToChunkWatchersAt(dimension,
                                  Game::Math::ChunkPos{packet.worldX >> 4, packet.worldZ >> 4},
                                  Network::PacketId::BlockChangeS2C, data);

            Log::Debug("[IntegratedServer] Sent block change at (%d, %d, %d) to block %d",
                      packet.worldX, packet.worldY, packet.worldZ, static_cast<int>(packet.newBlockId));
        }
    }

    void IntegratedServer::SendSectionBlocksUpdateS2CPacket(Game::DimensionId dimension,
                                                            const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
        // Check both that NetworkServer exists and we're not shutting down
        if (m_networkServer && !m_shouldStop.load()) {
            // TODO: Serialize the packet properly when serialization is implemented
            // For now, create a simple serialized format
            std::vector<uint8_t> data;
            
            // Write chunk coordinates
            data.push_back((packet.chunkPos.x >> 8) & 0xFF);
            data.push_back(packet.chunkPos.x & 0xFF);
            data.push_back((packet.chunkPos.z >> 8) & 0xFF);
            data.push_back(packet.chunkPos.z & 0xFF);
            
            // Write section Y
            data.push_back(packet.sectionY & 0xFF);
            
            // Write number of records as VarInt
            uint32_t recordCount = packet.packedRecords.size();
            while (recordCount > 127) {
                data.push_back((recordCount & 0x7F) | 0x80);
                recordCount >>= 7;
            }
            data.push_back(recordCount & 0x7F);
            
            // Write packed records. 64-bit VarInt — the record carries block id
            // + state index + position and no longer fits in 32 bits (see
            // ClientboundSectionBlocksUpdateS2CPacket). MC writes these as longs
            // for the same reason.
            for (uint64_t record : packet.packedRecords) {
                uint64_t val = record;
                while (val > 127) {
                    data.push_back((val & 0x7F) | 0x80);
                    val >>= 7;
                }
                data.push_back(val & 0x7F);
            }
            
            // Watcher-scoped for the same reason as the single-block path
            // above: the record stream is section-local coordinates, which
            // every client would happily apply to whichever world it is in.
            SendToChunkWatchersAt(dimension, packet.chunkPos,
                                  Network::PacketId::ClientboundSectionBlocksUpdate, data);

            Log::Debug("[IntegratedServer] Sent section block updates for chunk (%d, %d) section %d with %zu changes",
                      packet.chunkPos.x, packet.chunkPos.z, packet.sectionY, packet.packedRecords.size());
        }
    }

} // namespace Server