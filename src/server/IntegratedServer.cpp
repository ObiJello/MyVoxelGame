// File: src/server/IntegratedServer.cpp
#include "IntegratedServer.hpp"
#include "network/NetworkServer.hpp"
#include "network/ServerConnection.hpp"
#include "network/SendScheduler.hpp"
#include "session/PlayerSessionManager.hpp"
#include "session/PlayerSession.hpp"
#include "player/ServerPlayer.hpp"
#include "world/ticketing/ChunkTicketManager.hpp"
#include "world/watch/ChunkWatchIndex.hpp"
#include "world/status/ChunkStatusManager.hpp"
#include "world/tracking/SectionChangeAccumulator.hpp"
#include "world/tracking/ChunkDeltaBroadcaster.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/PacketTypes.hpp"

#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <future>
#include "common/world/level/World.hpp"
#include "client/entity/Player.hpp"
#include "world/ServerWorkerPool.hpp"
#include "world/MyTerrainGenerator.hpp"
#include "world/storage/SectionDataUnpacker.hpp"
#include "platform/GameDirectory.hpp"
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
        
        // Set Minecraft world path if provided in config
        if (!m_config.minecraftWorldPath.empty()) {
            m_world->SetMinecraftWorldPath(m_config.minecraftWorldPath);
            Log::Info("Server world configured with Minecraft world: %s", m_config.minecraftWorldPath.c_str());
        }
        
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
        m_networkServer = std::make_unique<NetworkServer>(*m_ioContext, 0);

        // Start NetworkServer on localhost (port 0 = OS picks a random available port)
        if (!m_networkServer->Start("127.0.0.1")) {
            Log::Error("Failed to start NetworkServer on 127.0.0.1");
            return false;
        }

        Log::Info("NetworkServer listening on 127.0.0.1:%d", m_networkServer->GetPort());
        
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
        using clock = std::chrono::steady_clock;
        using namespace std::chrono;
        
        static constexpr auto TICK_DURATION = 50ms;  // 20 TPS
        static constexpr auto SPIN_CUSHION = 2ms;    // Start spinning 2ms before target
        
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

        m_lastTickTime = clock::now();
        m_lastTickStartTime = m_lastTickTime;
        
        // Absolute scheduling - next tick time
        auto nextTickTime = clock::now() + TICK_DURATION;

        while (!m_shouldStop.load()) {
            // Wait until next tick (sleep for most of it)
            auto now = clock::now();
            if (now + SPIN_CUSHION < nextTickTime) {
                std::this_thread::sleep_until(nextTickTime - SPIN_CUSHION);
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

            PROFILE_FRAME_MARK_NAMED("ServerTick");

            // Advance to next tick using absolute schedule (no drift)
            nextTickTime += TICK_DURATION;
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
        
        // Tick the server player entity (authoritative gameplay)
        if (m_serverPlayer) {
            m_serverPlayer->tick(m_world.get(), static_cast<int>(serverTick));
        }

        // NOTE: PlayerSession is now ticked via m_sessionManager->Tick() at line 384
        // No need to tick it directly here anymore

        // === 2. SESSION-DRIVEN CHUNK LOADING (Minecraft-style) ===
        // Process watch set changes: request loading for new chunks entering view
        ProcessWatchSetChanges();

        // Process async chunk load results from ServerWorkerPool
        ProcessAsyncChunkResults();

        // === 3. RUN WORLD SIMULATION (no chunk loading — session system handles that) ===
        if (m_world) {
            m_world->WorldLoop(deltaTime);
        }

        // === 4. SEND CHUNKS per player (Minecraft's PlayerChunkSender pattern) ===
        if (m_sessionManager) {
            auto sessions = m_sessionManager->GetAllSessions();
            for (auto& session : sessions) {
                session->SendNextChunks(m_world.get());
            }
        }

        // === 5. PERIODIC CLEANUP: unload chunks with no watchers ===
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

        // Mark as pending
        m_pendingChunkLoads.insert(chunkPos);

        if (m_config.enableAsyncChunkLoading) {
            // Use ServerWorkerPool for async loading
            Threading::SubmitServerChunkLoading(chunkPos, priority);
            Log::Debug("Requested async chunk loading for (%d, %d)", chunkPos.x, chunkPos.z);
        } else {
            // Load synchronously via ChunkProvider
            if (m_world) {
                m_world->GetChunk(chunkPos.x, chunkPos.z); // Sync load into cache
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

                // Move chunk to ready-to-send for all sessions waiting for it
                if (m_sessionManager) {
                    auto sessions = m_sessionManager->GetAllSessions();
                    for (auto& session : sessions) {
                        if (session && session->IsWaitingForChunk(result.position)) {
                            session->MarkChunkReadyToSend(result.position);
                        }
                    }
                }

                Log::Debug("Async chunk ready (%d, %d)",
                         result.position.x, result.position.z);
            } else {
                // Mark as failed so it can be retried
                if (m_statusManager) {
                    m_statusManager->SetChunkStatus(result.position, Server::ChunkStatus::EMPTY);
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


    void IntegratedServer::ProcessWatchSetChanges() {
        PROFILE_ZONE;
        if (!m_sessionManager || !m_world) return;

        // Pump the terrain generator's async pipeline (like Minecraft's runDistanceManagerUpdates)
        auto* chunkProvider = m_world->GetChunkProvider();
        if (chunkProvider) {
            auto* generator = dynamic_cast<Game::MyTerrainGenerator*>(chunkProvider->GetGenerator());
            if (generator) {
                generator->PumpAsyncTasks();
            }
        }

        // Iterate pending chunk loads for each session
        auto sessions = m_sessionManager->GetAllSessions();
        for (const auto& session : sessions) {
            if (!session) continue;

            auto anchor = session->GetAnchorChunk();

            // Copy pending loads into a sortable vector
            std::vector<Game::Math::ChunkPos> pending(
                session->GetPendingChunkLoads().begin(),
                session->GetPendingChunkLoads().end());

            // Sort nearest-first so workers process close chunks before far ones
            std::sort(pending.begin(), pending.end(),
                [&anchor](const Game::Math::ChunkPos& a, const Game::Math::ChunkPos& b) {
                    int distA = std::max(std::abs(a.x - anchor.x), std::abs(a.z - anchor.z));
                    int distB = std::max(std::abs(b.x - anchor.x), std::abs(b.z - anchor.z));
                    return distA < distB;
                });

            // Process: move loaded chunks to ready-to-send, request loading for unloaded
            std::vector<Game::Math::ChunkPos> nowLoaded;
            for (const auto& pos : pending) {
                if (m_world->IsChunkLoaded(pos.x, pos.z)) {
                    nowLoaded.push_back(pos);
                } else if (m_pendingChunkLoads.count(pos) == 0) {
                    RequestChunkLoad(pos, 0);
                }
            }

            for (const auto& pos : nowLoaded) {
                session->MarkChunkReadyToSend(pos);
            }
        }
    }

    void IntegratedServer::UnloadUnwatchedChunks() {
        PROFILE_ZONE;
        if (!m_world || !m_watchIndex) return;

        auto* chunkProvider = m_world->GetChunkProvider();
        if (!chunkProvider) return;

        // Iterate only actually loaded chunks instead of scanning a huge grid
        auto loadedPositions = chunkProvider->GetLoadedChunkPositions();

        size_t unloaded = 0;
        for (const auto& pos : loadedPositions) {
            if (!m_watchIndex->HasWatchers(pos)) {
                if (chunkProvider->UnloadChunk(pos)) {
                    unloaded++;
                }
            }
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
            case Network::BlockActionType::BREAK:
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
        
        // Register connection with send scheduler
        if (m_sendScheduler) {
            m_sendScheduler->RegisterConnection(connection->GetConnectionId(), connection);
        }
        
        // Create session via SessionManager (new architecture)
        if (m_serverPlayer && m_sessionManager) {
            uint32_t playerId = 1;  // Single player ID for integrated server

            // Create session via SessionManager
            m_sessionManager->OnPlayerJoin(
                playerId,
                connection->GetConnectionId(),
                "Player"  // Default player name
            );

            // Attach ServerPlayer to the session created by manager
            auto session = m_sessionManager->GetSession(playerId);
            if (session) {
                session->AttachPlayer(m_serverPlayer.get());
                session->SetConnection(connection.get());
                Log::Info("[IntegratedServer] Player session created and wired to connection %u",
                          connection->GetConnectionId());

                // Send server-authoritative hotbar to client (Minecraft-style inventory sync)
                {
                    Network::HotbarSyncS2CPacket hotbarPacket;
                    for (int i = 0; i < 9; i++) {
                        hotbarPacket.slots[i] = static_cast<uint16_t>(m_serverPlayer->getHotbarBlock(i));
                    }
                    auto hotbarData = Network::Serialization::Serialize(hotbarPacket);
                    connection->SendPacket(static_cast<uint8_t>(Network::PacketId::HotbarSyncS2C), hotbarData);
                    Log::Info("[IntegratedServer] Sent hotbar sync to client");
                }
            } else {
                Log::Error("[IntegratedServer] Failed to retrieve session after OnPlayerJoin");
            }
        } else if (m_sessionManager) {
            // Fallback: Create session through manager
            uint32_t playerId = 1;  // Single player ID for integrated server
            m_sessionManager->OnPlayerJoin(
                playerId,
                connection->GetConnectionId(),
                "Player"  // Default player name
            );

            // Attach ServerPlayer to the session created by manager
            if (m_serverPlayer) {
                auto session = m_sessionManager->GetSession(playerId);
                if (session) {
                    session->AttachPlayer(m_serverPlayer.get());
                    Log::Info("[IntegratedServer] Attached ServerPlayer to session (fallback path)");
                } else {
                    Log::Error("[IntegratedServer] Failed to retrieve session in fallback path");
                }
            }

            Log::Info("[IntegratedServer] Player session created through manager");
        } else {
            Log::Warning("[IntegratedServer] Session manager not available, cannot handle player join");
        }
    }

    // ========================================================================
    // CLIENT SETTINGS
    // ========================================================================

    void IntegratedServer::OnClientSettingsReceived(uint32_t connectionId, int requestedViewDistance) {
        if (!m_sessionManager) return;

        auto session = m_sessionManager->GetSessionByConnection(connectionId);
        if (!session) {
            Log::Warning("[IntegratedServer] No session for connection %u", connectionId);
            return;
        }

        // Clamp client's requested distance to [2, serverViewDistance]
        int effectiveViewDistance = std::clamp(requestedViewDistance, 2, m_config.serverViewDistance);

        Log::Info("[IntegratedServer] Player %u requested view distance %d, effective: %d (server cap: %d)",
                  session->GetPlayerId(), requestedViewDistance, effectiveViewDistance, m_config.serverViewDistance);

        // Update session view distance (triggers watch set recalculation)
        session->SetViewDistance(effectiveViewDistance);

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

    void StartIntegratedServer() {
        if (g_integratedServer) {
            g_integratedServer->Start();
        }
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
        m_watchIndex = std::make_unique<ChunkWatchIndex>();
        m_statusManager = std::make_unique<ChunkStatusManager>();
        m_sendScheduler = std::make_unique<SendScheduler>();
        m_sessionManager = std::make_unique<PlayerSessionManager>();
        
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
        sessionConfig.worldSpawn = glm::vec3(0.0f, 67.0f, 0.0f);
        sessionConfig.spawnChunkRadius = 2;
        sessionConfig.maxChunksPerPlayerPerTick = m_config.maxChunksPerTick;
        sessionConfig.kickOnTimeout = false;  // Integrated server: never kick local player

        m_sessionManager->Initialize(
            sessionConfig,
            m_ticketManager.get(),
            m_watchIndex.get(),
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
            m_watchIndex.get()
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
        
        if (m_watchIndex) {
            // Remove all watchers for the integrated server player
            m_watchIndex->RemoveAllWatchersForPlayer(1);  // playerId=1 for integrated server
            m_watchIndex->Clear();
            m_watchIndex.reset();
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
            
            // Write packed records
            for (uint32_t record : packet.packedRecords) {
                // Write as VarInt
                uint32_t val = record;
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