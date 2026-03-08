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
#include "common/world/level/World.hpp"
#include "client/entity/Player.hpp"
#include "world/ServerWorkerPool.hpp"
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

        // IMPORTANT: Wait for server thread to finish BEFORE destroying resources
        // This prevents the server thread from accessing destroyed objects
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
        m_chunkSendStates.clear();
        m_pendingChunkLoads.clear();
        
        // Cleanup session management system
        CleanupSessionSystem();
        
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

    PlayerSession* IntegratedServer::GetPlayerSession() const {
        // Delegate to SessionManager (migrated from direct m_playerSession member)
        if (m_sessionManager) {
            auto session = m_sessionManager->GetSession(1); // playerId=1 for integrated server
            return session.get();
        }
        return nullptr;
    }

    Game::Math::ChunkPos IntegratedServer::GetPlayerChunkPosition() const {
        auto* session = GetPlayerSession();
        if (session) return session->GetChunkPosition();
        return {0, 0};
    }

    glm::vec3 IntegratedServer::GetPlayerPosition() const {
        auto* session = GetPlayerSession();
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

            // Advance to next tick using absolute schedule (no drift)
            nextTickTime += TICK_DURATION;
        }

        Log::Info("IntegratedServer main loop ended");
    }

    void IntegratedServer::ServerTick() {
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

        // Process async chunk load results from ServerWorkerPool
        ProcessAsyncChunkResults();
        
        // === 2. RUN WORLD SIMULATION ===
        // Minecraft runs runDistanceManagerUpdates + runGenerationTasks unconditionally
        // every tick with zero budget. haveTime is only used for unloading/saving.
        if (m_world) {
            m_world->WorldLoop(deltaTime, m_config.maxChunksPerTick);
        }
        
        // Send chunks to player with adaptive rate control (after WorldLoop, like Minecraft's tickChildren)
        SendChunksToPlayer();

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
        // Check if chunk is already loaded
        if (m_world && m_world->IsChunkLoaded(chunkPos.x, chunkPos.z)) {
            auto chunk = m_world->GetChunk(chunkPos.x, chunkPos.z);
            if (chunk) {
                SendChunkToClient(chunkPos, chunk);
                return;
            }
        }
        
        // Mark as pending
        m_pendingChunkLoads.insert(chunkPos);
        
        if (m_config.enableAsyncChunkLoading) {
            // Use ServerWorkerPool for async loading
            Threading::SubmitServerChunkLoading(chunkPos, priority);
            Log::Debug("Requested async chunk loading for (%d, %d)", chunkPos.x, chunkPos.z);
        } else {
            // Load synchronously (like vanilla Minecraft's integrated server)
            if (m_world) {
                m_world->UpdateLoadedChunks(chunkPos.x, chunkPos.z);
                auto chunk = m_world->GetChunk(chunkPos.x, chunkPos.z);
                if (chunk) {
                    SendChunkToClient(chunkPos, chunk);
                }
            }
        }
    }

    void IntegratedServer::SendChunkToClient(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk) {
        if (!chunk) {
            return;
        }

        // Check if chunk is already loaded by the client
        auto* session = GetPlayerSession();
        if (session && session->HasSentChunk(chunkPos)) {
            Log::Debug("Chunk (%d, %d) already loaded by client, skipping send", chunkPos.x, chunkPos.z);
            return;
        }
        
        // Build ChunkDataS2CPacket with full section data
        Network::ChunkDataS2CPacket packet;
        packet.chunkX = chunkPos.x;
        packet.chunkZ = chunkPos.z;
        packet.groundUpContinuous = true;
        packet.primaryBitmask = 0;

        // Serialize non-empty sections (same logic as World::ChunkEntityPacketDispatch)
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            const auto* section = chunk->GetSection(sectionY);
            if (!section) continue;

            uint16_t nonAirCount = 0;
            for (size_t i = 0; i < section->blocks.size(); ++i) {
                if (section->blocks[i] != static_cast<uint16_t>(Game::BlockID::Air)) {
                    nonAirCount++;
                }
            }
            if (nonAirCount == 0) continue;

            packet.primaryBitmask |= (1 << sectionY);

            Network::ChunkDataS2CPacket::SectionData sectionData;
            sectionData.blockCount = nonAirCount;
            sectionData.bitsPerEntry = 16; // Direct block IDs

            const size_t blocksPerSection = 16 * 16 * 16;
            const size_t blocksPerLong = 64 / 16; // 4 blocks per uint64_t
            sectionData.dataArray.resize((blocksPerSection + blocksPerLong - 1) / blocksPerLong, 0);

            for (size_t i = 0; i < blocksPerSection; ++i) {
                uint16_t blockId = section->blocks[i];
                size_t longIndex = i / blocksPerLong;
                size_t bitOffset = (i % blocksPerLong) * 16;
                sectionData.dataArray[longIndex] |= (static_cast<uint64_t>(blockId) << bitOffset);
            }

            packet.sections.push_back(std::move(sectionData));
        }

        SendChunkDataS2CPacket(std::move(packet));
        
        // Update send state
        ChunkSendState& sendState = m_chunkSendStates[chunkPos];
        sendState.chunkPos = chunkPos;
        sendState.sent = true;
        sendState.needsResend = false;
        sendState.sendTime = std::chrono::steady_clock::now();
        
        // Mark chunk as loaded by the client
        if (session) session->MarkChunkSent(chunkPos);
        
        // Register player as watcher for this chunk
        // This is critical for block change tracking to work!
        if (m_watchIndex) {
            m_watchIndex->AddWatcher(1, chunkPos);  // playerId=1 for integrated server
            Log::Info("[IntegratedServer] Registered player 1 as watcher for chunk (%d, %d)", chunkPos.x, chunkPos.z);
        } else {
            Log::Error("[IntegratedServer] m_watchIndex is null when trying to register watcher!");
        }
        
        m_stats.chunksSent.fetch_add(1, std::memory_order_relaxed);
        Log::Debug("Sent chunk (%d, %d) to client", chunkPos.x, chunkPos.z);
    }
    
    void IntegratedServer::QueueChunkForSending(Game::Math::ChunkPos chunkPos) {
        m_chunkSender.pendingChunks.insert(chunkPos);
    }

    void IntegratedServer::SendChunksToPlayer() {
        if (m_chunkSender.pendingChunks.empty()) return;

        // Back-pressure: don't send if too many unacknowledged batches
        if (m_chunkSender.unacknowledgedBatches >= m_chunkSender.maxUnacknowledgedBatches) {
            return;
        }

        // Accumulate fractional budget
        float maxBatchSize = std::max(1.0f, m_chunkSender.desiredChunksPerTick);
        m_chunkSender.batchQuota = std::min(
            m_chunkSender.batchQuota + m_chunkSender.desiredChunksPerTick,
            maxBatchSize
        );

        if (m_chunkSender.batchQuota < 1.0f) return;

        int maxBatch = static_cast<int>(m_chunkSender.batchQuota);

        // Collect closest chunks using partial sort when pending > maxBatch
        // Matches Minecraft's Comparators.least(maxBatchSize, comparator)
        struct ChunkDist {
            Game::Math::ChunkPos pos;
            int distSq;
        };
        std::vector<ChunkDist> candidates;
        candidates.reserve(m_chunkSender.pendingChunks.size());

        auto playerChunk = GetPlayerChunkPosition();
        for (const auto& pos : m_chunkSender.pendingChunks) {
            int dx = pos.x - playerChunk.x;
            int dz = pos.z - playerChunk.z;
            candidates.push_back({pos, dx * dx + dz * dz});
        }

        size_t toSend = std::min(static_cast<size_t>(maxBatch), candidates.size());
        if (toSend == 0) return;

        if (candidates.size() > toSend) {
            std::partial_sort(candidates.begin(), candidates.begin() + toSend, candidates.end(),
                              [](const ChunkDist& a, const ChunkDist& b) { return a.distSq < b.distSq; });
        } else {
            std::sort(candidates.begin(), candidates.end(),
                      [](const ChunkDist& a, const ChunkDist& b) { return a.distSq < b.distSq; });
        }

        // Build the batch — look up chunk data from cache at send time
        std::vector<std::pair<Game::Math::ChunkPos, std::shared_ptr<Game::Chunk>>> batch;
        for (size_t i = 0; i < toSend; ++i) {
            auto chunk = m_world->GetChunkProvider()->GetChunk(candidates[i].pos);
            if (chunk) {
                batch.push_back({candidates[i].pos, chunk});
            }
        }

        if (batch.empty()) return;

        // Send batch start
        {
            auto data = Network::Serialization::Serialize(Network::ChunkBatchStartS2CPacket{});
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::ChunkBatchStartS2C), data);
        }

        // Send each chunk
        for (const auto& [pos, chunk] : batch) {
            SendChunkToClient(pos, chunk);
            m_chunkSender.pendingChunks.erase(pos);
        }

        // Send batch finished
        {
            Network::ChunkBatchFinishedS2CPacket finishPacket(static_cast<int32_t>(batch.size()));
            auto data = Network::Serialization::Serialize(finishPacket);
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::ChunkBatchFinishedS2C), data);
        }

        m_chunkSender.batchQuota -= static_cast<float>(batch.size());
        m_chunkSender.unacknowledgedBatches++;

        Log::Debug("Sent chunk batch: %zu chunks (quota=%.1f, unacked=%d, rate=%.1f)",
                  batch.size(), m_chunkSender.batchQuota,
                  m_chunkSender.unacknowledgedBatches, m_chunkSender.desiredChunksPerTick);
    }

    void IntegratedServer::OnChunkBatchAck(float desiredRate) {
        m_chunkSender.unacknowledgedBatches--;
        m_chunkSender.desiredChunksPerTick = std::isnan(desiredRate)
            ? ChunkSenderState::MIN_RATE
            : std::clamp(desiredRate, ChunkSenderState::MIN_RATE, ChunkSenderState::MAX_RATE);
        if (m_chunkSender.unacknowledgedBatches == 0) {
            m_chunkSender.batchQuota = 1.0f;
        }
        m_chunkSender.maxUnacknowledgedBatches = ChunkSenderState::MAX_UNACKED_BATCHES;

        Log::Debug("Chunk batch ack: desiredRate=%.2f, unacked=%d, maxUnacked=%d",
                  m_chunkSender.desiredChunksPerTick, m_chunkSender.unacknowledgedBatches,
                  m_chunkSender.maxUnacknowledgedBatches);
    }

    void IntegratedServer::ProcessAsyncChunkResults() {
        // Only process if async chunk loading is enabled
        if (!m_config.enableAsyncChunkLoading) {
            return;
        }

        auto& resultQueue = Threading::ServerWorkerPool::GetChunkGenResultQueue();

        // Use time budget instead of fixed count — process results until budget exhausted
        auto startTime = std::chrono::steady_clock::now();
        const float budgetMs = 5.0f;
        int resultsProcessed = 0;

        Network::ChunkGenResult result;
        while (resultQueue.try_pop(result)) {
            // Check time budget
            if (resultsProcessed > 0) {
                auto now = std::chrono::steady_clock::now();
                float elapsedMs = std::chrono::duration<float, std::milli>(now - startTime).count();
                if (elapsedMs >= budgetMs) break;
            }

            if (result.success && result.chunk) {
                // Chunk is already in ChunkProvider cache (worker called GetChunk -> CompleteChunkLoad)
                // Update status tracking
                if (m_statusManager) {
                    m_statusManager->MarkChunkReady(result.position);
                }

                // Send to client
                SendChunkToClient(result.position, result.chunk);

                Log::Debug("Processed async chunk load for (%d, %d) - success",
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
            // Fallback to old system if session manager not available
            Log::Warning("[IntegratedServer] Session manager not available, using legacy chunk sending");
            
            // Get spawn position (player's initial position)
            glm::vec3 spawnPos = GetPlayerPosition();
            if (spawnPos.x == 0.0f && spawnPos.z == 0.0f) {
                spawnPos = glm::vec3(0.0f, 67.0f, 0.0f); // Default spawn
            }
            
            // Calculate chunk position
            auto centerChunk = Game::Math::WorldCoordinates::WorldToChunkPos(
                static_cast<int>(spawnPos.x), 
                static_cast<int>(spawnPos.z)
            );
            
            Log::Info("[IntegratedServer] Player spawn chunk: (%d, %d)", centerChunk.x, centerChunk.z);
            
            // Send chunks in a radius around the spawn position
            int chunkRadius = Platform::g_gameSettings.GetRenderDistance(); // Use actual render distance setting
            int chunksSent = 0;
        
            for (int dx = -chunkRadius; dx <= chunkRadius; dx++) {
                for (int dz = -chunkRadius; dz <= chunkRadius; dz++) {
                    Game::Math::ChunkPos chunkPos(centerChunk.x + dx, centerChunk.z + dz);
                    
                    // Calculate priority based on Chebyshev distance (square pattern like Minecraft)
                    // Chebyshev distance = max(abs(dx), abs(dz))
                    int chebyshevDistance = std::max(std::abs(dx), std::abs(dz));
                    int priority = chunkRadius - chebyshevDistance;
                    
                    // Request chunk loading (will send to client once loaded)
                    RequestChunkLoad(chunkPos, priority);
                    chunksSent++;
                }
            }
            
            Log::Info("[IntegratedServer] Requested %d chunks for new player", chunksSent);
        }  // End of else block for legacy chunk sending
    }

    // ========================================================================
    // PACKET SENDING
    // ========================================================================

    void IntegratedServer::SendChunkDataS2CPacket(Network::ChunkDataS2CPacket&& packet) {
        // Check both that NetworkServer exists and we're not shutting down
        if (m_networkServer && !m_shouldStop.load()) {
            // Log::Info("[IntegratedServer] SendChunkDataS2CPacket: chunk (%d, %d) with bitmask 0x%X, sections: %zu",
            //           packet.chunkX, packet.chunkZ, packet.primaryBitmask, packet.sections.size());
            
            auto data = Network::Serialization::Serialize(packet);
            
            // // Log first few bytes of serialized data
            // std::string hexDump;
            // for (size_t i = 0; i < std::min(size_t(20), data.size()); i++) {
            //     char buf[4];
            //     snprintf(buf, sizeof(buf), "%02X ", data[i]);
            //     hexDump += buf;
            // }
            // Log::Debug("[IntegratedServer] Serialized chunk data (first 20 bytes): %s", hexDump.c_str());
            
            // Log::Debug("[IntegratedServer] Broadcasting packet ID 0x%02X (ChunkDataS2C), data size: %zu",
            //           static_cast<uint8_t>(Network::PacketId::ChunkDataS2C), data.size());
            
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::ChunkDataS2C), data);
            m_stats.packetsSent.fetch_add(1, std::memory_order_relaxed);
            m_stats.chunksSent.fetch_add(1, std::memory_order_relaxed);
        } else {
            Log::Warning("[IntegratedServer] Cannot send chunk: NetworkServer=%p, shouldStop=%d",
                        m_networkServer.get(), m_shouldStop.load());
        }
    }

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
        int renderDistance = Platform::g_gameSettings.GetRenderDistance();
        
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
        if (!m_watchIndex) {
            return;
        }
        
        // Get current required chunks based on view distance
        auto requiredChunks = GetRequiredChunks();
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> requiredSet(
            requiredChunks.begin(), requiredChunks.end()
        );
        
        // Find chunks to remove (in sent states but not in required)
        std::vector<Game::Math::ChunkPos> toRemove;
        for (const auto& [chunk, state] : m_chunkSendStates) {
            if (requiredSet.find(chunk) == requiredSet.end()) {
                toRemove.push_back(chunk);
            }
        }

        // Remove watchers for chunks going out of view
        for (const auto& chunk : toRemove) {
            m_watchIndex->RemoveWatcher(1, chunk);  // playerId=1 for integrated server
            m_chunkSendStates.erase(chunk);
            Log::Debug("Removed watcher for chunk (%d, %d) - out of view distance", chunk.x, chunk.z);
        }
        
        // Note: Adding watchers is handled in SendChunkToClient when chunks are sent
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
        auto* session = GetPlayerSession();
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
        sessionConfig.defaultViewDistance = m_config.defaultViewDistance;  // Use view distance in chunks
        sessionConfig.worldSpawn = glm::vec3(0.0f, 67.0f, 0.0f);
        sessionConfig.spawnChunkRadius = 2;
        sessionConfig.maxChunksPerPlayerPerTick = m_config.maxChunksPerTick;
        
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