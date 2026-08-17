// File: src/server/IntegratedServer.cpp
#include "IntegratedServer.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "commands/TeleportCommand.hpp"
#include "commands/KickCommand.hpp"
#include "commands/GameModeCommand.hpp"
#include "commands/KillCommand.hpp"
#include "commands/SummonCommand.hpp"
#include "commands/SheepEatCommand.hpp"
#include "commands/TimeCommand.hpp"
#include "commands/GameRuleCommand.hpp"
#include "commands/SeedCommand.hpp"
#include "commands/TickCommand.hpp"
#include "network/NetworkServer.hpp"
#include "network/ServerConnection.hpp"
#include "network/SendScheduler.hpp"
#include "session/PlayerSessionManager.hpp"
#include "session/PlayerSession.hpp"
#include "player/ServerPlayer.hpp"
#include "level/PlayerSpawnFinder.hpp"
#include "world/ticketing/ChunkTicketManager.hpp"
#include "world/status/ChunkStatusManager.hpp"
#include "world/tracking/SectionChangeAccumulator.hpp"
#include "world/tracking/ChunkDeltaBroadcaster.hpp"
#include "entity/ItemEntityManager.hpp"
#include "entity/MobManager.hpp"
#include "entity/ServerLevelBridge.hpp"
#include "entity/ServerEntityTracker.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/world/spawn/NaturalSpawner.hpp"
#include "common/physics/Physics.hpp"
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
#include <future>
#include "common/world/level/World.hpp"
#include "client/entity/Player.hpp"
#include "world/ServerWorkerPool.hpp"
#include "world/MyTerrainGenerator.hpp"
#include "world/storage/SectionDataUnpacker.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN
#include "portal/PortalRegistry.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <thread>

namespace Server {

    // Global instance
    std::unique_ptr<IntegratedServer> g_integratedServer = nullptr;
    
    // Server thread ID for assertions
    std::thread::id g_serverThreadId;

    IntegratedServer::IntegratedServer(const IntegratedServerConfig& config)
        : m_config(config) {
        m_tickDuration = std::chrono::duration<float>(1.0f / static_cast<float>(m_config.tickRate));
        Log::Info("IntegratedServer created with %d TPS", m_config.tickRate);
    }

    IntegratedServer::~IntegratedServer() {
        if (m_running.load()) {
            Stop();
        }
        Log::Info("IntegratedServer destroyed");
    }

    bool IntegratedServer::Initialize() {
        Log::Info("IntegratedServer::Initialize - Creating world on server thread");

        // serverViewDistance stays at default (32) for integrated server — no cap on client.
        // A dedicated server would set this from its config file.
        Log::Info("Server view distance cap: %d chunks", m_config.serverViewDistance);

        // Create the world instance owned by the server
        m_world = std::make_unique<Game::World>();
        
        // Set Minecraft world path if provided in config.
        // Read-only MUST be set before World::Initialize() — that is where the
        // chunk provider (and its saver, or lack of one) gets built.
        m_world->SetReadOnly(m_config.readOnlyWorld);
        if (!m_config.minecraftWorldPath.empty()) {
            m_world->SetMinecraftWorldPath(m_config.minecraftWorldPath);
            Log::Info("Server world configured with Minecraft world: %s%s",
                      m_config.minecraftWorldPath.c_str(),
                      m_config.readOnlyWorld ? " (read-only)" : "");
        }
        
        // Restore world time + gamerule from world metadata (worlds.json)
        m_world->SetDayTime(m_config.initialDayTime);
        m_world->SetDoDaylightCycle(m_config.doDaylightCycle);

        // Initialize the world
        m_world->Initialize();
        Log::Info("Server world initialized successfully");

        // Initialize the new session management system
        InitializeSessionSystem();

        // Create ServerPlayer instance (PlayerSession will be created when player joins)
        glm::vec3 spawnPos(0.0f, 67.0f, 0.0f);
        m_serverPlayer = std::make_unique<ServerPlayer>(1, "Player");
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

        if (!m_world) {
            Log::Error("Cannot start IntegratedServer without world");
            return false;
        }

        Log::Info("Starting IntegratedServer thread...");

        // Create io_context and NetworkServer
        m_ioContext = std::make_unique<net::io_context>();
        m_networkServer = std::make_unique<NetworkServer>(*m_ioContext, 25565);

        // Start NetworkServer on all interfaces so remote clients can connect
        if (!m_networkServer->Start("0.0.0.0")) {
            Log::Error("Failed to start NetworkServer on 0.0.0.0:25565");
            return false;
        }

        Log::Info("NetworkServer listening on 0.0.0.0:%d", m_networkServer->GetPort());

        // Register server commands (MC: Commands.java constructor)
        TeleportCommand::Register(m_commandDispatcher);
        KickCommand::Register(m_commandDispatcher);
        GameModeCommand::Register(m_commandDispatcher);
        KillCommand::Register(m_commandDispatcher);
        SummonCommand::Register(m_commandDispatcher);
        SheepEatCommand::Register(m_commandDispatcher);
        TimeCommand::Register(m_commandDispatcher);
        GameRuleCommand::Register(m_commandDispatcher);
        SeedCommand::Register(m_commandDispatcher);
        TickCommand::Register(m_commandDispatcher);
        Log::Info("Server commands registered");

        // Wire disconnect callback so we broadcast entity removal to other clients
        m_networkServer->SetOnDisconnection([this](std::shared_ptr<ServerConnection> conn) {
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

        // Tell the world to abort any long-running chunk loading loops
        if (m_world) {
            m_world->RequestStop();
        }

        // IMPORTANT: Wait for server thread to finish BEFORE destroying resources.
        // World::RequestStop() signals the terrain library's abort flag, so blocking
        // getChunk() loops will exit promptly.
        if (m_serverThread && m_serverThread->joinable()) {
            Log::Debug("Waiting for server thread to finish...");
            m_serverThread->join();
            Log::Debug("Server thread finished");
        }
        m_serverThread.reset();

        // Stop the network I/O thread
        if (m_ioContext && m_networkThread) {
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
        
        // Cleanup session system BEFORE destroying network resources.
        // Sessions and SendScheduler hold ServerConnection shared_ptrs with
        // Asio strands — these must be destroyed while io_context is still alive.
        CleanupSessionSystem();

        // Now it's safe to destroy NetworkServer and io_context
        // since both the server thread and I/O thread are no longer running
        if (m_networkServer) {
            m_networkServer->Stop();
            m_networkServer.reset();
        }

        if (m_ioContext) {
            m_ioContext.reset();
        }

        m_running.store(false);
        Log::Info("IntegratedServer stopped");
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
        m_pendingChunkLoads.clear();

        // Note: CleanupSessionSystem() already called in Stop() before io_context destruction

        // Shutdown and release the world
        if (m_world) {
            Log::Info("Shutting down server-owned world...");
            m_world->SaveAllChunks();
            m_world->Shutdown();
            m_world.reset();
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
        
        Log::Info("IntegratedServer main loop started (Target: %d TPS, ThreadId: %zu)",
                  m_config.tickRate, std::hash<std::thread::id>{}(g_serverThreadId));

        // Initialize chunk provider on server thread so ServerChunkCache
        // captures the correct main thread ID (matching Minecraft's architecture
        // where the server thread owns all chunk data structures)
        if (m_world) {
            m_world->InitializeChunkProvider();
        }

        // ── World spawn selection (MC MinecraftServer.setInitialSpawn) ─────
        // Generated worlds ask the generator: climate SpawnFinder picks the
        // region, a chunk spiral finds dry land, getBaseHeight supplies the
        // surface Y. Anvil worlds keep the legacy fixed spawn for now (MC
        // reads theirs from level.dat, which we don't parse yet). Runs before
        // any client can join, so every player — host and remote — spawns
        // (and is teleported on join) to this position.
        if (m_config.minecraftWorldPath.empty() && m_world && m_world->GetChunkProvider()) {
            if (auto* generator = m_world->GetChunkProvider()->GetGenerator()) {
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
                auto* provider = m_world->GetChunkProvider();
                const int spawnChunkX = static_cast<int>(std::floor(spawnBlock.x / 16.0f));
                const int spawnChunkZ = static_cast<int>(std::floor(spawnBlock.z / 16.0f));
                int xOff = 0, zOff = 0, dx = 0, dz = -1;
                bool resolved = false;
                for (int i = 0; i < 11 * 11 && !resolved; ++i) {
                    if (xOff >= -5 && xOff <= 5 && zOff >= -5 && zOff <= 5) {
                        const Game::Math::ChunkPos probe(spawnChunkX + xOff, spawnChunkZ + zOff);
                        if (provider->GetChunk(probe)) {
                            if (auto found = PlayerSpawnFinder::GetSpawnPosInChunk(*m_world, probe)) {
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
                    m_worldSpawn = PlayerSpawnFinder::FixupSpawnHeight(*m_world, spawnBlock);
                    Log::Warning("[IntegratedServer] No standable spawn within 5 chunks — "
                                 "height-corrected estimate to (%.1f, %.1f, %.1f)",
                                 m_worldSpawn.x, m_worldSpawn.y, m_worldSpawn.z);
                }
                Log::Info("[IntegratedServer] World spawn set to (%.1f, %.1f, %.1f)",
                          m_worldSpawn.x, m_worldSpawn.y, m_worldSpawn.z);
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

        m_lastTickTime = clock::now();
        m_lastTickStartTime = m_lastTickTime;
        
        // Absolute scheduling - next tick time
        auto nextTickTime = clock::now() + nanoseconds(m_tickRateManager.nanosecondsPerTick());

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
            const auto pumpUntil = nextTickTime - SPIN_CUSHION;
            while (!m_shouldStop.load() && clock::now() < pumpUntil) {
                PumpChunkPipeline(pumpUntil);

                // PumpChunkPipeline only returns before the deadline when the
                // pipeline is idle, so there is genuinely nothing to do until
                // something lands on the queue.
                if (clock::now() < pumpUntil) {
                    if (auto* gen = GetTerrainGenerator()) {
                        gen->WaitForPipelineWork(pumpUntil);
                    } else {
                        std::this_thread::sleep_until(pumpUntil);
                    }
                }
            }

            // Micro-spin for the final stretch to land exactly on time
            while (clock::now() < nextTickTime) {
                std::this_thread::yield();
            }
            
            auto tickStart = clock::now();
            
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


            // Feeds /tick query's P50/P95/P99, and its "lagging" test.
            m_tickRateManager.recordTickTime(
                duration_cast<nanoseconds>(tickEnd - tickStart).count());
            if (sprinting) {
                m_tickRateManager.endTickWork();
            }

            PROFILE_FRAME_MARK_NAMED("ServerTick");

            // Advance to next tick using absolute schedule (no drift). A
            // sprinting tick adds nothing, so the next iteration starts
            // immediately.
            nextTickTime += tickBudget;
        }

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

        // Network I/O is now handled by dedicated thread (no need to poll)
        // The I/O thread runs continuously and processes all async operations
        
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
        
        // Process the new session management system FIRST
        // This updates watch sets before broadcasting block changes
        if (m_sessionManager) {
            // Tick all player sessions (updates watch sets)
            m_sessionManager->Tick(serverTick);

            // Process expired tickets
            if (m_ticketManager) {
                m_ticketManager->ProcessExpiredTickets(serverTick);
            }

            // Process send queues
            if (m_sendScheduler) {
                m_sendScheduler->ProcessSendQueues();
            }
        }

        // === 3. FLUSH ACCUMULATED BLOCK CHANGES ===
        // This MUST happen AFTER session tick (so watch sets are updated)
        // All block changes from this tick are broadcast to watchers now
        if (m_deltaBroadcaster) {
            m_deltaBroadcaster->flush();
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
        if (m_tickRateManager.runsNormally()) {
            Game::Portal::ServerRegistry().Tick(this);
        }
#endif

        // === 2. SESSION-DRIVEN CHUNK LOADING (Minecraft-style) ===
        // Process watch set changes: request loading for new chunks entering view
        ProcessWatchSetChanges();

        // Process async chunk load results from ServerWorkerPool
        ProcessAsyncChunkResults();

        // === 3. RUN WORLD SIMULATION (no chunk loading — session system handles that) ===
        //
        // Gated on runsNormally() — MC's `/tick freeze`. Only the SIMULATION
        // stops: packet draining, session ticks, chunk streaming and the delta
        // broadcast above all keep running, so a frozen server stays joinable,
        // keeps sending chunks, and still answers commands (including
        // `/tick unfreeze`). That is exactly the split vanilla makes, and
        // freezing the whole tick instead would lock the player out of the
        // server they just froze.
        if (m_world && m_tickRateManager.runsNormally()) {
            // Hand the world this tick's simulation set before it runs. MC does
            // the equivalent inside ServerChunkCache.tickChunks, which walks
            // ChunkMap's block-ticking chunks; World deliberately can't reach
            // for the ticket manager itself, so the server pushes it in.
            //
            // Recomputed every tick because players move. It is a walk of the
            // ticket manager's level cache, which is already maintained for
            // other reasons — not a fresh distance computation per chunk.
            if (m_ticketManager) {
                m_world->SetBlockTickingChunks(m_ticketManager->GetBlockTickingChunks());
            }
            m_world->WorldLoop(deltaTime);

            // Dropped items. Inside the runsNormally() gate on purpose: `/tick
            // freeze` should stop items mid-air and stop their despawn clock,
            // exactly like every other piece of world simulation.
            if (m_itemEntities) {
                PROFILE_ZONE_N("ItemEntityTick");
                std::vector<int32_t> removed;
                std::vector<ItemPickupEvent> pickups;
                m_itemEntities->Tick(m_world.get(), m_sessionManager.get(),
                                     removed, pickups);
                // Pickups first: the take packet is what retires a collected
                // entity client-side, and it has to arrive while the client
                // still has the entity to animate.
                if (!pickups.empty()) {
                    BroadcastItemEntityPickups(pickups);
                }
                if (!removed.empty()) {
                    BroadcastItemEntityRemovals(removed);
                }
                BroadcastItemEntityUpdates(serverTick);
            }

            // Mobs. Same gate as items — `/tick freeze` must stop AI, physics
            // and the despawn clock together, or an unfrozen mob walks through
            // a frozen world.
            TickMobs(serverTick);
        }

        // === 4. SEND CHUNKS per player (Minecraft's PlayerChunkSender pattern) ===
        if (m_sessionManager) {
            auto sessions = m_sessionManager->GetAllSessions();
            for (auto& session : sessions) {
                session->SendNextChunks(m_world.get());
            }
        }

        // === 5. BROADCAST PLAYER POSITIONS to other players (~10 Hz) ===
        if (m_sessionManager && (serverTick % 2 == 0)) {
            m_sessionManager->BroadcastPlayerPositions();
        }

        // === 5b. TIME SYNC every 20 ticks (MC MinecraftServer.tickChildren) ===
        if (serverTick % 20 == 0) {
            ForceTimeSync();
        }

        // === 6. PERIODIC CLEANUP: unload chunks with no watchers ===
        if (serverTick % 60 == 0) { // Every ~3 seconds at 20 TPS
            UnloadUnwatchedChunks();
        }

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

    void IntegratedServer::RequestChunkLoad(Game::Math::ChunkPos chunkPos, int priority) {
        // Already loaded — SendNextChunks() will pick it up
        if (m_world && m_world->IsChunkLoaded(chunkPos.x, chunkPos.z)) {
            return;
        }

        // Already in flight — a result is coming, and it will push the chunk to
        // every tracking player. Submitting again would generate it twice.
        if (m_pendingChunkLoads.count(chunkPos) != 0) {
            return;
        }

        // m_pendingChunkLoads is the "already in flight, don't submit again"
        // guard, and the ONLY thing that clears an entry is a result arriving in
        // ProcessAsyncChunkResults. So a chunk may be marked pending only once a
        // job that is guaranteed to produce a result actually exists —
        // otherwise the entry never clears, the guard suppresses every future
        // request, and that chunk never loads again for the rest of the session.
        // (Symptom: a world loads its spawn chunks and then stops, with new
        // chunks streaming normally once you walk into positions that weren't
        // poisoned.)
        if (m_config.enableAsyncChunkLoading) {
            if (!Threading::SubmitServerChunkLoading(chunkPos, priority)) {
                // Only reachable when the pool is shutting down — the queue has
                // no capacity limit. Leaving the chunk unmarked is correct:
                // there is no result coming, and nothing is left to retry into.
                return;
            }
            m_pendingChunkLoads.insert(chunkPos);
            Log::Debug("Requested async chunk loading for (%d, %d)", chunkPos.x, chunkPos.z);
        } else {
            // Sync path: the chunk is in the cache by the time this returns, so
            // push it to its watchers right here — ProcessAsyncChunkResults
            // early-outs entirely when async loading is off, so nothing else
            // ever would.
            if (m_world && m_world->GetChunk(chunkPos.x, chunkPos.z) && m_sessionManager) {
                m_sessionManager->ForEachSessionWatching(
                    chunkPos,
                    [&](PlayerSession& session) { session.MarkChunkPendingToSend(chunkPos); });
            }
        }
    }

    // ========================================================================
    // ITEM ENTITY BROADCAST
    // ========================================================================

    void IntegratedServer::BroadcastItemEntitySpawn(int32_t id) {
        if (!m_itemEntities || !m_sessionManager) return;

        const auto& all = m_itemEntities->All();
        auto it = all.find(id);
        if (it == all.end()) return;
        const Game::ItemEntity& e = it->second;

        Network::ItemEntitySpawnS2CPacket packet;
        packet.entityId = e.id;
        packet.position = e.pos;
        packet.velocity = glm::vec3(e.vel);
        packet.bobOffs  = e.bobOffs;
        packet.stack    = e.stack;

        const auto data = Network::Serialization::Serialize(packet);
        SendToChunkWatchers(e.pos, Network::PacketId::ItemEntitySpawnS2C, data);
    }

    // ========================================================================
    // MOB ENTITIES
    // ========================================================================

    namespace {
        // Factory shared by the spawner and the debug spawn command.
        std::unique_ptr<Game::Mob> MakeMob(Game::EntityTypeId type, Game::EntityLevel* level) {
            switch (type) {
                case Game::EntityTypeId::Zombie:   return std::make_unique<Game::Zombie>(level);
                case Game::EntityTypeId::Skeleton: return std::make_unique<Game::Skeleton>(level);
                case Game::EntityTypeId::Creeper:  return std::make_unique<Game::Creeper>(level);
                case Game::EntityTypeId::Spider:   return std::make_unique<Game::Spider>(level);
                case Game::EntityTypeId::Cow:      return std::make_unique<Game::Cow>(level);
                case Game::EntityTypeId::Pig:      return std::make_unique<Game::Pig>(level);
                case Game::EntityTypeId::Sheep:    return std::make_unique<Game::Sheep>(level);
                case Game::EntityTypeId::Chicken:  return std::make_unique<Game::Chicken>(level);
                default: break;
            }
            // Everything else is built from its generated def. Promoting one to
            // a hand-written class is purely additive: add a case above and the
            // generic path stops being used for it.
            return Game::MakeGenericMob(type, level);
        }
    }

    void IntegratedServer::TickMobs(int64_t serverTick) {
        if (!m_mobs || !m_mobLevel || !m_mobTracker || !m_sessionManager) return;

        PROFILE_ZONE_N("MobSystemTick");

        // 1. Refresh the player views FIRST. Everything downstream — targeting,
        //    despawn distance, the tracker's watch sets — reads them, and a
        //    stale view is a dangling ServerPlayer pointer.
        m_mobLevel->SyncPlayerViews();

        // 2. Tick, scoped to the block-ticking chunk set (mobs outside it are
        //    still despawn-checked; see MobManager::Tick).
        std::vector<int32_t> removed;

        // The ticket manager hands out a vector of ChunkPos; MobManager wants a
        // set it can probe per mob, so pack to the same 64-bit key its index
        // uses. Built once per tick rather than per mob.
        std::unordered_set<uint64_t> tickingChunks;
        if (m_ticketManager) {
            const auto chunkList = m_ticketManager->GetBlockTickingChunks();
            tickingChunks.reserve(chunkList.size());
            for (const auto& pos : chunkList) {
                tickingChunks.insert((static_cast<uint64_t>(static_cast<uint32_t>(pos.x)) << 32) |
                                      static_cast<uint32_t>(pos.z));
            }
        }
        m_mobs->Tick(tickingChunks, removed);

        // 3. Spawn. After ticking so this tick's despawns have already freed
        //    room under the caps.
        RunNaturalSpawner(serverTick);

        // 4. Emit. The tracker owns who-knows-what, so removals go through it
        //    rather than being broadcast blindly.
        std::vector<EntityPacketOut> outgoing;
        for (int32_t id : removed) m_mobTracker->RemoveEntity(id, outgoing);

        std::vector<std::pair<uint32_t, glm::dvec3>> players;
        for (const auto& session : m_sessionManager->GetAllSessions()) {
            if (!session || !session->GetPlayer()) continue;
            players.emplace_back(session->GetConnectionId(), session->GetPlayer()->getPosition());
        }

        m_mobTracker->Tick(*m_mobs, *m_mobLevel, players, outgoing);

        // 5. Send. One lookup per recipient rather than per packet — a busy
        //    tick emits hundreds of packets across a handful of connections.
        for (const auto& packet : outgoing) {
            auto session = m_sessionManager->GetSession(packet.connectionId);
            if (!session || !session->GetConnection()) continue;
            session->GetConnection()->SendPacket(static_cast<uint8_t>(packet.packetId),
                                                 packet.payload);
        }

        // 6. Knockback the mob system applied to players. The player is
        //    client-authoritative for movement, so a push has to be SENT as a
        //    velocity packet or the next move packet simply overwrites it.
        for (Server::PlayerEntityView* view : m_mobLevel->PlayerViews()) {
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

    void IntegratedServer::RunNaturalSpawner(int64_t serverTick) {
        if (!m_mobs || !m_mobLevel || !m_world || !m_ticketManager) return;

        // MC ServerLevel.tickChunk gates the spawner on doMobSpawning. Only the
        // spawner — despawning and AI keep running, so the world drains rather
        // than freezing when it is turned off.
        if (!m_world->GetDoMobSpawning()) return;

        // Loaded ones only. GetBlockTickingChunks reports every chunk inside
        // simulation distance, loaded or not, and the spawner probes biome and
        // surface height inside each — reads that answer nothing useful for a
        // chunk with no data, and used to drag a blocking generate along with
        // them. MC's spawnable set is its loaded, ticking holders, so this is
        // also the more faithful count for the mob-cap maths below.
        auto tickingChunks = m_ticketManager->GetBlockTickingChunks();
        tickingChunks.erase(
            std::remove_if(tickingChunks.begin(), tickingChunks.end(),
                           [this](const Game::Math::ChunkPos& cp) {
                               return !m_world->IsChunkLoaded(cp.x, cp.z);
                           }),
            tickingChunks.end());
        if (tickingChunks.empty()) return;

        Game::SpawnContext ctx;
        ctx.level = m_mobLevel.get();
        ctx.spawnableChunkCount = static_cast<int>(tickingChunks.size());
        m_mobs->SetSpawnableChunkCount(ctx.spawnableChunkCount);

        int categoryCounts[8] = {};
        for (int i = 0; i < 8; ++i) categoryCounts[i] = m_mobs->CountForCategory(i);
        ctx.categoryCounts = categoryCounts;

        std::vector<glm::dvec3> playerPositions;
        for (Server::PlayerEntityView* view : m_mobLevel->PlayerViews()) {
            if (view->IsSpectator()) continue;
            playerPositions.push_back(view->position);
        }
        if (playerPositions.empty()) return;   // nobody to spawn for
        ctx.playerPositions = &playerPositions;

        Game::World* world = m_world.get();
        ctx.biomeAt = [world](int x, int y, int z) -> std::string_view {
            // BiomeInfo::name is the bare vanilla slug ("plains"), which is
            // exactly the key GeneratedMobSpawns is built on — so no
            // translation table is needed between the two.
            return Game::BiomeRegistry::Get(world->GetBiome(x, y, z)).name;
        };

        // MC Level.noCollision(type.getSpawnAABB(...)).
        ctx.spawnBoxFree = [world](Game::EntityTypeId type,
                                   double x, double y, double z) -> bool {
            const Game::EntityTypeInfo& info = Game::GetEntityTypeInfo(type);
            const float half = info.width * 0.5f;
            Game::AABB box;
            box.min = glm::vec3(x - half, y, z - half);
            box.max = glm::vec3(x + half, y + info.height, z + half);

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

        // MC isRightDistanceToPlayerAndSpawnPoint's respawn-point half.
        if (m_sessionManager) {
            const glm::vec3 spawn = m_sessionManager->GetPlayerSpawn(0);
            ctx.worldSpawn = glm::dvec3(spawn);
            ctx.hasWorldSpawn = true;
        }

        // MC LocalMobCapCalculator. A chunk may spawn a category only if SOME
        // player within spawn range of it is below maxInstancesPerChunk for
        // that category — counted per player, over the mobs near that player.
        // Without it a single crowded area starves spawning everywhere else,
        // because only the global cap would apply.
        MobManager* mobs = m_mobs.get();
        ctx.canSpawnLocal = [mobs, &playerPositions](Game::MobCategory category,
                                                     int chunkX, int chunkZ) -> bool {
            if (!mobs) return true;

            const int maxPerChunk = Game::GetMobCategoryInfo(category).maxInstancesPerChunk;
            const double chunkCenterX = chunkX * 16.0 + 8.0;
            const double chunkCenterZ = chunkZ * 16.0 + 8.0;

            // MC's "close for spawning" radius is the 8-chunk spawn distance.
            constexpr double kSpawnRangeSq =
                (Game::kSpawnDistanceChunk * 16.0) * (Game::kSpawnDistanceChunk * 16.0);

            for (const glm::dvec3& p : playerPositions) {
                const double dx = p.x - chunkCenterX;
                const double dz = p.z - chunkCenterZ;
                if (dx * dx + dz * dz > kSpawnRangeSq) continue;

                int nearCount = 0;
                for (const auto& [id, mob] : mobs->All()) {
                    if (mob->TypeInfo().category != category) continue;
                    const double mx = mob->position.x - p.x;
                    const double mz = mob->position.z - p.z;
                    if (mx * mx + mz * mz <= kSpawnRangeSq) ++nearCount;
                }
                if (nearCount < maxPerChunk) return true;
            }

            // Either no player is in range, or every one of them is already at
            // the cap. MC's canSpawn returns false in both cases.
            return false;
        };

        Game::EntityLevel* level = m_mobLevel.get();
        ctx.createMob = [level](Game::EntityTypeId type) { return MakeMob(type, level); };

        // MC shuffles the spawnable chunk list and walks it. Walking it in a
        // fixed order would bias spawning toward whichever chunks happen to
        // hash first, which shows up as mobs clustering on one side of a player.
        std::vector<Game::Math::ChunkPos> chunks(tickingChunks.begin(), tickingChunks.end());
        Game::JavaRandom& rng = m_mobLevel->Random();
        for (size_t i = chunks.size(); i > 1; --i) {
            std::swap(chunks[i - 1], chunks[rng.NextInt(static_cast<int>(i))]);
        }

        std::vector<std::unique_ptr<Game::Mob>> spawned;
        for (const auto& pos : chunks) {
            // MC hands spawnForChunk the resolved LevelChunk. We filtered the
            // list to loaded chunks above, so this is a cache hit.
            auto chunk = m_world->GetLoadedChunk(pos.x, pos.z);
            if (!chunk) continue;
            // MC's heightmaps are always live; ours can be unprimed on a chunk
            // that skipped both the generator copy and the NBT restore, and
            // getRandomPosWithin would then sample against MIN_Y.
            if (!chunk->AreHeightmapsPrimed()) chunk->PrimeHeightmaps();
            Game::SpawnForChunk(ctx, *chunk, pos.x, pos.z, serverTick, rng, spawned);
        }

        for (auto& mob : spawned) m_mobs->Add(std::move(mob));
    }

    int IntegratedServer::SummonMobs(Game::EntityTypeId type, const glm::dvec3& pos, int count) {
        if (!m_mobs || !m_mobLevel) return 0;

        int spawned = 0;
        Game::JavaRandom& rng = m_mobLevel->Random();

        for (int i = 0; i < count; ++i) {
            std::unique_ptr<Game::Mob> mob = MakeMob(type, m_mobLevel.get());
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

            // MC SummonCommand calls finalizeSpawn whenever the summon carries
            // no NBT overriding it — which is every summon this engine has.
            mob->FinalizeSpawn();

            // Summoned mobs never despawn — MC does the same for /summon, and
            // it is what makes the command usable for testing at all.
            mob->SetPersistenceRequired(true);

            if (m_mobs->Add(std::move(mob)) != 0) ++spawned;
        }
        return spawned;
    }

    bool IntegratedServer::SpawnMobFromItemUse(Game::EntityTypeId type,
                                               const glm::ivec3& spawnPos,
                                               bool tryMoveDown, bool movedUp) {
        if (!m_mobs || !m_mobLevel || !m_world) return false;

        // MC SpawnEggItem.spawnMob's own gate, before anything is created.
        const Game::EntityTypeInfo& info = Game::GetEntityTypeInfo(type);
        if (info.notInPeaceful &&
            m_mobLevel->GetDifficulty() == Game::Difficulty::Peaceful) {
            return false;
        }

        std::unique_ptr<Game::Mob> mob = MakeMob(type, m_mobLevel.get());
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
            phys.blockAccess = m_world.get();
            std::vector<Game::AABBd> colliders;
            Game::CollectBlockColliders(Game::ToAABBd(region), phys, colliders);

            const double desired = movedUp ? -2.0 : -1.0;
            yOffset = 1.0 + Game::CollideAxis(1, Game::ToAABBd(mob->GetAABB()),
                                              desired, colliders);
        }

        mob->position = glm::dvec3(spawnPos.x + 0.5, spawnPos.y + yOffset, spawnPos.z + 0.5);
        mob->yRot = Game::Mth::WrapDegrees(m_mobLevel->Random().NextFloat() * 360.0f);
        mob->xRot = 0.0f;
        mob->yHeadRot = mob->yRot;
        mob->yBodyRot = mob->yRot;

        // MC EntityType.create runs finalizeSpawn for SPAWN_ITEM_USE too, which
        // is why a vanilla spawn egg can produce a grey, brown or (rarely) pink
        // sheep rather than always a white one.
        mob->FinalizeSpawn();

        // MC does NOT mark egg-spawned mobs persistent — they despawn like any
        // natural spawn. /summon is the one that pins them (see SummonMobs).
        return m_mobs->Add(std::move(mob)) != 0;
    }

    void IntegratedServer::HandleInteract(uint32_t connectionId, int32_t entityId,
                                          bool attack, bool sprinting) {
        if (!m_mobs || !m_mobLevel) return;

        Server::PlayerEntityView* attacker = m_mobLevel->GetPlayerView(connectionId);
        if (!attacker || attacker->IsSpectator()) return;

        // The id space already separates the two (see Game::kMobEntityIdBase):
        // players are connection ids below kItemEntityIdBase, mobs above
        // kMobEntityIdBase. Dispatching on it is what lets one packet carry
        // both PvE and PvP without a kind byte.
        Game::LivingEntity* target = nullptr;
        Game::Mob*          mobTarget = nullptr;
        if (Game::IsMobEntityId(entityId)) {
            mobTarget = m_mobs->Find(entityId);
            target = mobTarget;
        } else if (entityId >= 0 && entityId < Game::kItemEntityIdBase) {
            // A player. Never yourself: MC's pick never returns the attacker,
            // but a hand-made packet could.
            if (static_cast<uint32_t>(entityId) == connectionId) return;
            target = m_mobLevel->GetPlayerView(static_cast<uint32_t>(entityId));
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
        if (target->GetAABBd().DistanceToSqr(attacker->GetEyePosition()) >= kMaxRangeSq) return;

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
            if (!mobTarget) return;   // only mobs have a mobInteract
            ServerPlayer* player = attacker->GetPlayer();
            if (!player) return;

            const int slot = player->getInventory().GetSelectedSlot();
            Game::ItemStack& held = player->getInventory().MutableSlot(
                Game::Inventory::HotbarToIndex(slot));

            const bool creative = (player->getGameMode() == GameMode::CREATIVE);
            const Game::ItemStack before = held;

            // MC restores only the COUNT in creative — see the same note on
            // the block-use path in PlayerSession::HandleUseItemOn, where
            // restoring the whole stack silently wiped a component.
            const auto restoreCreative = [&] {
                if (!creative) return;
                if (held.IsEmpty()) held = before;
                else                held.count = before.count;
            };

            Game::UseResult r = mobTarget->MobInteract(*attacker, held);

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
                auto session = m_sessionManager
                    ? m_sessionManager->GetSession(connectionId) : nullptr;
                if (session) session->SendInventoryFull();
            }
            return;
        }

        // ── ATTACK — MC Player.attack, in its order ────────────────────────
        ServerPlayer* player = attacker->GetPlayer();
        if (!player) return;

        const Game::ItemStack& held = player->getInventory().GetSlot(
            Game::Inventory::HotbarToIndex(player->getInventory().GetSelectedSlot()));

        float itemDamage = 0.0f, itemSpeed = 0.0f;
        Game::GetItemAttackAttributes(held.itemId, itemDamage, itemSpeed);

        // MC reads ATTACK_DAMAGE, which is the player's base plus the held
        // item's main-hand modifier. A bare hand is 1.0.
        float damage = Game::kPlayerBaseAttackDamage + itemDamage;

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

        // MC Player.canCriticalAttack — every term of it:
        //   fallDistance > 0, !onGround, !onClimbable, !isInWater,
        //   !isMobilityRestricted, !isPassenger, target is a LivingEntity,
        //   !isSprinting.
        //
        // fallDistance > 0 && !onGround is why a crit is a hit taken on the way
        // DOWN — the accumulator only grows while descending (see
        // PlayerSession::UpdateMovementStats). isMobilityRestricted and
        // isPassenger have no analogue here: no status effects, no vehicles.
        const bool crit = fullStrength
                       && player->getFallDistance() > 0.0f
                       && !player->isOnGround()
                       && !IsOnClimbable(*player)
                       && !attacker->IsInWater()
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

        const bool hit = target->Hurt(Game::MobDamageSource::PlayerAttack, damage, attacker);
        if (hit) {
            attacker->SetLastHurtMob(target);
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
            if (sweep) DoSweepAttack(*attacker, *target, strengthScale);
            BroadcastAttackEffects(*target, crit);

            // MC Player.attack's last line: 0.1 exhaustion per landed hit.
            // causeFoodExhaustion no-ops for an invulnerable player, which is
            // how creative and spectator are excluded — the same gate the
            // movement exhaustion sources use in PlayerSession.
            const GameMode mode = player->getGameMode();
            if (mode == GameMode::SURVIVAL || mode == GameMode::ADVENTURE) {
                player->getFoodData().addExhaustion(0.1f);
            }
        }
    }

    bool IntegratedServer::IsOnClimbable(const ServerPlayer& player) const {
        if (!m_world) return false;
        const glm::dvec3 pos = player.getPosition();
        const Game::BlockID block = m_world->GetBlock(
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
        if (!m_mobLevel) return;

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
        m_mobLevel->GetEntitiesInBox(box, &attacker, nearby);

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
        return m_mobLevel ? m_mobLevel->GetPlayerView(connectionId) : nullptr;
    }

    void IntegratedServer::BroadcastAttackEffects(const Game::LivingEntity& target,
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
        SendToChunkWatchersAt(cp, Network::PacketId::EntityEventS2C, data);
    }

    void IntegratedServer::BroadcastItemEntityUpdates(int64_t serverTick) {
        if (!m_itemEntities || !m_sessionManager) return;

        std::vector<int32_t> fullRefresh;
        std::vector<int32_t> moveOnly;
        m_itemEntities->CollectSyncSets(serverTick, fullRefresh, moveOnly);

        for (int32_t id : fullRefresh) {
            BroadcastItemEntitySpawn(id);
        }

        if (moveOnly.empty()) return;

        // Compact updates are bucketed by chunk so each client gets ONE packet
        // covering everything it can see, rather than one per entity.
        const auto& all = m_itemEntities->All();
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
            SendToChunkWatchersAt(chunk, Network::PacketId::ItemEntityMoveS2C, data);
        }
    }

    void IntegratedServer::BroadcastItemEntityPickups(
            const std::vector<ItemPickupEvent>& pickups) {
        if (pickups.empty() || !m_networkServer) return;

        // Unscoped, like removals: a client that never knew the entity simply
        // finds nothing to animate and ignores the packet.
        for (const auto& p : pickups) {
            Network::TakeItemEntityS2CPacket packet;
            packet.itemEntityId = p.itemEntityId;
            packet.playerId     = p.playerId;
            packet.amount       = p.amount;

            const auto data = Network::Serialization::Serialize(packet);
            m_networkServer->BroadcastPacket(
                static_cast<uint8_t>(Network::PacketId::TakeItemEntityS2C), data);
        }
    }

    void IntegratedServer::BroadcastItemEntityRemovals(const std::vector<int32_t>& ids) {
        if (ids.empty() || !m_networkServer) return;

        // Unscoped on purpose — see the header note. A client that never knew
        // the entity just doesn't find the id in its map.
        Network::RemoveEntitiesS2CPacket packet(ids);
        const auto data = Network::Serialization::Serialize(packet);
        m_networkServer->BroadcastPacket(
            static_cast<uint8_t>(Network::PacketId::EntityDestroy), data);
    }

    void IntegratedServer::SendToChunkWatchers(const glm::dvec3& pos,
                                              Network::PacketId packetId,
                                              const std::vector<uint8_t>& data) {
        const Game::Math::ChunkPos cp{
            static_cast<int32_t>(std::floor(pos.x / 16.0)),
            static_cast<int32_t>(std::floor(pos.z / 16.0))
        };
        SendToChunkWatchersAt(cp, packetId, data);
    }

    void IntegratedServer::SendToChunkWatchersAt(Game::Math::ChunkPos chunk,
                                                Network::PacketId packetId,
                                                const std::vector<uint8_t>& data) {
        if (!m_sessionManager) return;

        m_sessionManager->ForEachSessionWatching(chunk, [&](PlayerSession& session) {
            if (auto* conn = session.GetConnection()) {
                conn->SendPacket(static_cast<uint8_t>(packetId), data);
            }
        });
    }

    void IntegratedServer::ProcessAsyncChunkResults() {
        PROFILE_ZONE;
        // Only process if async chunk loading is enabled
        if (!m_config.enableAsyncChunkLoading) {
            return;
        }

        auto& resultQueue = Threading::ServerWorkerPool::GetChunkGenResultQueue();

        // Process ALL completed results this tick (no time budget — let the send rate be the throttle)
        int resultsProcessed = 0;

        Network::ChunkGenResult result;
        while (resultQueue.try_pop(result)) {

            if (result.success && result.chunk) {
                // Chunk is already in ChunkProvider cache (worker called GetChunk -> CompleteChunkLoad)
                // Update status tracking
                if (m_statusManager) {
                    m_statusManager->MarkChunkReady(result.position);
                }

                // MC ChunkMap.onChunkReadyToSend: the chunk PUSHES itself to
                // every player already tracking it. Nobody polls for it, and no
                // session keeps a "waiting for this chunk" list — the tracking
                // view is the only membership test.
                if (m_sessionManager) {
                    m_sessionManager->ForEachSessionWatching(
                        result.position,
                        [&](PlayerSession& session) {
                            session.MarkChunkPendingToSend(result.position);
                        });
                }

                Log::Debug("Async chunk ready (%d, %d)",
                         result.position.x, result.position.z);
            } else {
                // Mark as failed so it can be retried
                if (m_statusManager) {
                    m_statusManager->SetChunkStatus(result.position, Server::ChunkStatus::EMPTY);
                }
                // Erasing from m_pendingChunkLoads below is the whole retry
                // mechanism: the chunk is still inside somebody's tracking view,
                // so RetryFailedChunkLoads re-requests it on a later tick.
                if (m_sessionManager) {
                    m_sessionManager->ForEachSessionWatching(
                        result.position,
                        [&](PlayerSession&) { m_failedChunkLoads.insert(result.position); });
                }
                Log::Warning("Async chunk load failed for (%d, %d): %s",
                           result.position.x, result.position.z,
                           result.errorMessage.c_str());
            }

            // Remove from pending list
            m_pendingChunkLoads.erase(result.position);
            resultsProcessed++;
        }

        if (resultsProcessed > 0) {
            Log::Debug("Processed %d async chunk results this tick", resultsProcessed);
        }
    }


    Game::MyTerrainGenerator* IntegratedServer::GetTerrainGenerator() const {
        if (!m_world) return nullptr;
        auto* chunkProvider = m_world->GetChunkProvider();
        if (!chunkProvider) return nullptr;
        return dynamic_cast<Game::MyTerrainGenerator*>(chunkProvider->GetGenerator());
    }

    void IntegratedServer::PumpChunkPipeline(std::chrono::steady_clock::time_point deadline) {
        auto* generator = GetTerrainGenerator();
        if (!generator) return;

        PROFILE_ZONE_N("PumpChunkPipeline");

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
        // Breaking out early when PumpOneTask() returns false is MC's
        // waitForTasks(): there is nothing left to do, so give the time back to
        // the caller rather than spinning on an idle pipeline.
        while (std::chrono::steady_clock::now() < deadline) {
            if (!generator->PumpOneTask()) break;
        }
    }

    void IntegratedServer::ProcessWatchSetChanges() {
        PROFILE_ZONE;
        if (!m_sessionManager || !m_world) return;

        // Pump the terrain generator's async pipeline (like Minecraft's
        // runDistanceManagerUpdates). Kept here as well as in the loop's idle
        // window because the watch-set scan below enqueues NEW requests, and
        // draining them right away starts generation this tick rather than next.
        //
        // Budgeted at one tick, which is MC's own overrun allowance for
        // main-thread work (MinecraftServer:762 —
        // `delayedTasksMaxNextTickTimeNanos = max(now + thisTickNanos,
        // nextTickTimeNanos)`). Worst case this tick runs long; it cannot run
        // unbounded, and the chunk send below still happens this tick.
        PumpChunkPipeline(std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(m_tickRateManager.nanosecondsPerTick()));

        // Re-request anything whose load came back failed. Normally a no-op.
        if (!m_failedChunkLoads.empty()) {
            auto failed = std::move(m_failedChunkLoads);
            m_failedChunkLoads.clear();
            for (const auto& pos : failed) {
                RequestChunkLoad(pos, 0);
            }
        }

        // Refresh the positions the worker pool prioritises against. MC
        // re-evaluates its queue level per poll for the same reason: a request
        // burst ordered by where the player stood when it was issued is wrong
        // by the time the workers get to the far half of it.
        {
            std::vector<Game::Math::ChunkPos> anchors;
            for (const auto& session : m_sessionManager->GetAllSessions()) {
                if (session) anchors.push_back(session->GetAnchorChunk());
            }
            Threading::SetServerChunkLoadAnchors(std::move(anchors));
        }

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
        for (const auto& session : sessions) {
            if (!session) continue;

            session->UpdateChunkTracking(
                [&](Game::Math::ChunkPos pos) {
                    // MC markChunkPendingToSend: getChunkToSend() returns null
                    // for a chunk that is not loaded, and the call quietly does
                    // nothing. Ours asks the world the same question.
                    if (m_world->IsChunkLoaded(pos.x, pos.z)) {
                        session->MarkChunkPendingToSend(pos);
                    } else {
                        RequestChunkLoad(pos, 0);
                    }
                },
                [&](Game::Math::ChunkPos pos) {
                    session->DropChunk(pos);
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
        if (!m_world || !m_sessionManager) return;

        auto* chunkProvider = m_world->GetChunkProvider();
        if (!chunkProvider) return;

        // Iterate only actually loaded chunks instead of scanning a huge grid
        auto loadedPositions = chunkProvider->GetLoadedChunkPositions();

        // "Watched" is now asked of the tracking views directly — the same
        // question the reverse index used to answer, minus the index.
        const auto sessions = m_sessionManager->GetAllSessions();
        auto anyoneTracking = [&sessions](Game::Math::ChunkPos pos) {
            for (const auto& session : sessions) {
                if (session && session->GetTrackingView().Contains(pos)) return true;
            }
            return false;
        };

        size_t unloaded = 0;
        std::vector<int32_t> removedItems;
        for (const auto& pos : loadedPositions) {
            if (!anyoneTracking(pos)) {
                if (chunkProvider->UnloadChunk(pos)) {
                    unloaded++;
                    // Dropped items go with their chunk. Item entities are
                    // memory-only by design (no entity storage in the save
                    // format), and leaving them behind would have them ticking
                    // against blocks that no longer exist — they'd fall forever
                    // through a world with no floor.
                    if (m_itemEntities) {
                        m_itemEntities->RemoveInChunk(pos, removedItems);
                    }
                    // Mobs go the same way, and for the same reason: there is
                    // no entity persistence layer, so a mob left in an unloaded
                    // chunk would tick against blocks that no longer exist.
                    if (m_mobs) {
                        std::vector<int32_t> removedMobs;
                        m_mobs->RemoveInChunk(pos, removedMobs);
                        if (!removedMobs.empty() && m_mobTracker && m_sessionManager) {
                            std::vector<EntityPacketOut> outgoing;
                            for (int32_t id : removedMobs) {
                                m_mobTracker->RemoveEntity(id, outgoing);
                            }
                            for (const auto& packet : outgoing) {
                                auto session = m_sessionManager->GetSession(packet.connectionId);
                                if (!session || !session->GetConnection()) continue;
                                session->GetConnection()->SendPacket(
                                    static_cast<uint8_t>(packet.packetId), packet.payload);
                            }
                        }
                    }
                }
            }
        }

        if (!removedItems.empty()) {
            BroadcastItemEntityRemovals(removedItems);
        }

        if (unloaded > 0) {
            Log::Info("Unloaded %zu unwatched chunks", unloaded);
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
        if (!m_world) {
            return;
        }
        
        // Apply change to world
        bool success = m_world->SetBlock(worldX, worldY, worldZ, blockId);
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
                glm::ivec3(worldX, worldY, worldZ));
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

        // Default name = "PlayerN" using connection id (matches MC's offline-mode pattern).
        // The default is always unique because connection ids are monotonically increasing
        // and never reused by an existing session.
        std::string defaultName = "Player" + std::to_string(playerId);

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

        // Determine which ServerPlayer to use:
        // - Connection 1 (host): use existing m_serverPlayer
        // - Other connections: create a new ServerPlayer
        ServerPlayer* playerPtr = nullptr;
        if (playerId == 1 && m_serverPlayer) {
            playerPtr = m_serverPlayer.get();
            // Host's m_serverPlayer was constructed with the placeholder "Player" before we
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

            // Apply client settings that arrived before this session existed.
            // Without this the session keeps its starting view distance of 2 and
            // the player gets a small square of chunks that only expands when
            // they cross a chunk boundary (which recomputes the watch set for
            // unrelated reasons). See OnClientSettingsReceived.
            const uint32_t connId = connection->GetConnectionId();
            auto pendingVd = m_pendingClientViewDistance.find(connId);
            if (pendingVd != m_pendingClientViewDistance.end()) {
                Log::Info("[IntegratedServer] Applying deferred client view distance %d for connection %u",
                          pendingVd->second, connId);
                ApplyClientViewDistance(*session, connId, pendingVd->second);
                m_pendingClientViewDistance.erase(pendingVd);
            }
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
            playerPtr->setGameMode(static_cast<GameMode>(m_config.defaultGameMode));
            connection->SendPlayerAbilities(*playerPtr);

            // Send full 46-slot inventory snapshot. Replaces the old HotbarSyncS2C path —
            // InventoryFullS2C carries real per-slot counts so the client doesn't have to
            // synthesize them.
            session->SendInventoryFull();
            Log::Info("[IntegratedServer] Sent full inventory sync to client");

            // Teleport the new client to the player's server-side position
            // (world spawn for fresh players). The client boots at its own
            // hardcoded coords; without this, it stays there no matter what
            // spawn the server picked. Mirrors MC PlayerList.placeNewPlayer's
            // connection.teleport(...) on login.
            {
                const glm::dvec3 pos = playerPtr->getPosition();
                connection->Teleport(pos.x, pos.y, pos.z, 0.0f, 0.0f);
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

    void IntegratedServer::OnPlayerDisconnected(std::shared_ptr<ServerConnection> connection) {
        uint32_t connectionId = connection->GetConnectionId();
        uint32_t playerId = connection->GetPlayerId();
        std::string playerName = connection->GetPlayerName();

        Log::Info("[IntegratedServer] Player '%s' (ID: %u, conn: %u) disconnected",
                  playerName.c_str(), playerId, connectionId);

        // Drop any client settings still waiting on a session that will now
        // never exist — a reused connection id must not inherit them.
        m_pendingClientViewDistance.erase(connectionId);

        // Forget everything this player was tracking. Connection ids ARE
        // reused, so a leftover watch set would make the next player to take
        // this id silently never receive spawn packets for those mobs.
        if (m_mobTracker) m_mobTracker->RemovePlayer(connectionId);

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

        // 2. Clean up remote ServerPlayer
        m_remotePlayers.erase(playerId);

        // 3. Unregister from SendScheduler
        if (m_sendScheduler) {
            m_sendScheduler->UnregisterConnection(connectionId);
        }

        // 4. Clean up session (watch index, chunk state, etc.)
        if (m_sessionManager) {
            m_sessionManager->OnPlayerLeave(playerId, "Disconnected");
        }
    }

    // ========================================================================
    // CLIENT SETTINGS
    // ========================================================================

    void IntegratedServer::OnClientSettingsReceived(uint32_t connectionId, int requestedViewDistance) {
        // Note the ordering: a missing session manager must still stash, not
        // return. Bailing out before the stash would lose the settings exactly
        // when they are most likely to arrive early — during startup.
        auto session = m_sessionManager
            ? m_sessionManager->GetSessionByConnection(connectionId)
            : nullptr;
        if (!session) {
            // The client sends ClientConfigC2S once, straight after LoginSuccess,
            // and it can beat OnPlayerJoined to the server. Dropping it here used
            // to be permanent: nothing resends it, so the session stayed at its
            // starting view distance of 2 (PlayerSessionManager: "Initialize at
            // minimum view distance, like Minecraft's ServerPlayer default") and
            // the player got a tiny square of chunks that never grew.
            //
            // It only appeared to fix itself on movement because crossing a
            // chunk boundary moves the tracking view's centre, which makes
            // UpdateChunkTracking re-diff with whatever view distance had been
            // applied by then — hence "nothing loads, then the whole render
            // distance arrives at once".
            //
            // Stash it instead; OnPlayerJoined applies it as soon as the session
            // exists.
            Log::Info("[IntegratedServer] Client settings for connection %u arrived before its "
                      "session — deferring view distance %d to join",
                      connectionId, requestedViewDistance);
            m_pendingClientViewDistance[connectionId] = requestedViewDistance;
            return;
        }

        ApplyClientViewDistance(*session, connectionId, requestedViewDistance);
    }

    void IntegratedServer::ApplyClientViewDistance(PlayerSession& session,
                                                   uint32_t connectionId,
                                                   int requestedViewDistance) {
        // Clamp client's requested distance to [2, serverViewDistance]
        int effectiveViewDistance = std::clamp(requestedViewDistance, 2, m_config.serverViewDistance);

        Log::Info("[IntegratedServer] Player %u requested view distance %d, effective: %d (server cap: %d)",
                  session.GetPlayerId(), requestedViewDistance, effectiveViewDistance, m_config.serverViewDistance);

        // Update session view distance (triggers watch set recalculation)
        session.SetViewDistance(effectiveViewDistance);

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
        Log::Info("Server State: TPS=%.1f, TickTime=%.2fms, LoadedChunks=%zu, PendingLoads=%zu",
                 m_stats.averageTPS.load(), m_stats.averageTickTime.load(),
                 sentChunks, m_pendingChunkLoads.size());
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
        
        // Create core components
        m_ticketManager = std::make_unique<ChunkTicketManager>();
        m_statusManager = std::make_unique<ChunkStatusManager>();
        m_sendScheduler = std::make_unique<SendScheduler>();
        m_sessionManager = std::make_unique<PlayerSessionManager>();
        m_itemEntities = std::make_unique<ItemEntityManager>();

        // Mobs. The bridge is constructed first because the manager holds a
        // pointer to it, and the bridge needs the manager back for entity
        // queries — hence the explicit SetMobManager rather than a constructor
        // argument.
        m_mobLevel = std::make_unique<ServerLevelBridge>(m_world.get(), m_sessionManager.get());
        m_mobs = std::make_unique<MobManager>(m_mobLevel.get());
        m_mobLevel->SetMobManager(m_mobs.get());
        m_mobTracker = std::make_unique<ServerEntityTracker>();

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
        sessionConfig.defaultSimulationDistance = 8;
        sessionConfig.defaultViewDistance = m_config.defaultViewDistance;
        sessionConfig.maxViewDistance = m_config.serverViewDistance;  // Server's view distance cap
        // Placeholder at init time — the server thread recomputes the real
        // spawn at startup and pushes it via SetWorldSpawn (see Start()).
        sessionConfig.worldSpawn = m_worldSpawn;
        sessionConfig.spawnChunkRadius = 2;
        sessionConfig.maxChunksPerPlayerPerTick = m_config.maxChunksPerTick;
        sessionConfig.kickOnTimeout = false;  // Integrated server: never kick local player

        m_sessionManager->Initialize(
            sessionConfig,
            m_ticketManager.get(),
            m_statusManager.get(),
            m_sendScheduler.get()
        );
        
        // Add spawn chunk tickets
        Game::Math::ChunkPos spawnChunk(0, 0);
        m_ticketManager->AddSpawnTickets(spawnChunk, 2);
        
        // Initialize block change accumulation and broadcasting
        m_changeAccumulator = std::make_unique<SectionChangeAccumulator>();
        m_deltaBroadcaster = std::make_unique<ChunkDeltaBroadcaster>(
            this,
            m_changeAccumulator.get(),
            m_sessionManager.get()
        );
        
        Log::Info("Session management system initialized successfully");
    }

    void IntegratedServer::CleanupSessionSystem() {
        Log::Info("Cleaning up session management system...");
        
        // Clean up broadcaster and accumulator first
        if (m_deltaBroadcaster) {
            m_deltaBroadcaster.reset();
        }
        
        if (m_changeAccumulator) {
            m_changeAccumulator.reset();
        }
        
        if (m_sessionManager) {
            m_sessionManager->Shutdown();
            m_sessionManager.reset();
        }
        
        if (m_sendScheduler) {
            m_sendScheduler->Shutdown();
            m_sendScheduler.reset();
        }
        
        if (m_statusManager) {
            m_statusManager->Clear();
            m_statusManager.reset();
        }
        
        if (m_ticketManager) {
            m_ticketManager->Clear();
            m_ticketManager.reset();
        }
        
        Log::Info("Session management system cleaned up");
    }
    
    void IntegratedServer::SendBlockChangeS2CPacket(const Network::BlockChangeS2CPacket& packet) {
        // Check both that NetworkServer exists and we're not shutting down
        if (m_networkServer && !m_shouldStop.load()) {
            auto data = Network::Serialization::Serialize(packet);
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::BlockChangeS2C), data);
            
            Log::Debug("[IntegratedServer] Sent block change at (%d, %d, %d) to block %d",
                      packet.worldX, packet.worldY, packet.worldZ, static_cast<int>(packet.newBlockId));
        }
    }
    
    void IntegratedServer::SendSectionBlocksUpdateS2CPacket(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
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
            
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::ClientboundSectionBlocksUpdate), data);
            
            Log::Debug("[IntegratedServer] Sent section block updates for chunk (%d, %d) section %d with %zu changes",
                      packet.chunkPos.x, packet.chunkPos.z, packet.sectionY, packet.packedRecords.size());
        }
    }

} // namespace Server