// File: src/server/IntegratedServer.cpp
#include "IntegratedServer.hpp"
#include "network/NetworkServer.hpp"
#include "network/ServerConnection.hpp"
#include "network/SendScheduler.hpp"
#include "session/PlayerSessionManager.hpp"
#include "session/PlayerSession.hpp"
#include "world/ticketing/ChunkTicketManager.hpp"
#include "world/watch/ChunkWatchIndex.hpp"
#include "world/status/ChunkStatusManager.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/PacketTypes.hpp"

#include "common/core/Log.hpp"
#include "common/world/level/World.hpp"
#include "client/input/PlayerController.hpp"
#include "world/ServerWorkerPool.hpp"
#include "world/storage/SectionDataUnpacker.hpp"
#include "platform/GameDirectory.hpp"
#include <algorithm>
#include <thread>

namespace Server {

    // Global instance
    std::unique_ptr<IntegratedServer> g_integratedServer = nullptr;

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

        // Initialize player state (for compatibility)
        m_playerState.playerId = 1; // Single player ID for integrated server
        m_playerState.position = glm::vec3(0.0f, 67.0f, 0.0f);
        m_playerState.currentChunk = Game::Math::WorldCoordinates::WorldToChunkPos(0, 0);
        m_playerState.lastUpdateTime = std::chrono::steady_clock::now();

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
        
        // Start NetworkServer on localhost
        if (!m_networkServer->Start("127.0.0.1")) {
            Log::Error("Failed to start NetworkServer on 127.0.0.1:25565");
            return false;
        }
        
        Log::Info("NetworkServer listening on 127.0.0.1:25565");
        
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

    void IntegratedServer::SetPlayerController(Game::PlayerController* playerController) {
        m_playerController = playerController;
        
        if (playerController) {
            // Update player state from controller
            const auto& physics = playerController->GetPhysics();
            m_playerState.position = physics.position;
            m_playerState.currentChunk = Game::Math::WorldCoordinates::WorldToChunkPos(
                static_cast<int>(physics.position.x), 
                static_cast<int>(physics.position.z)
            );
        }
    }

    void IntegratedServer::UpdatePlayerState(const Network::PlayerMoveC2SPacket& packet) {
        m_playerState.position = packet.position;
        m_playerState.rotation = packet.rotation;
        m_playerState.lastMoveSequenceNumber = packet.sequenceNumber;
        m_playerState.lastUpdateTime = std::chrono::steady_clock::now();
        
        // Update current chunk
        auto newChunk = Game::Math::WorldCoordinates::WorldToChunkPos(
            static_cast<int>(packet.position.x), 
            static_cast<int>(packet.position.z)
        );
        
        if (newChunk.x != m_playerState.currentChunk.x || newChunk.z != m_playerState.currentChunk.z) {
            UpdatePlayerChunkPosition(packet.position);
        }
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
        
        Log::Info("IntegratedServer main loop started (Target: %d TPS)", m_config.tickRate);
        
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
        
        // Process the new session management system
        if (m_sessionManager) {
            // Tick all player sessions
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
        
        // Process async chunk load results from ServerWorkerPool
        ProcessAsyncChunkResults();
        
        // Call World's unified tick loop with chunk send throttling
        if (m_world) {
            // Use time budget for chunk processing
            auto chunkStartTime = std::chrono::steady_clock::now();
            int chunksProcessed = 0;
            
            // Process up to maxChunksPerTick or until time budget exceeded
            while (chunksProcessed < m_config.maxChunksPerTick) {
                auto currentTime = std::chrono::steady_clock::now();
                float elapsedMs = std::chrono::duration<float, std::milli>(currentTime - chunkStartTime).count();
                
                if (elapsedMs >= m_config.chunkProcessBudgetMs) {
                    break; // Time budget exceeded
                }
                
                // Process one chunk worth of work
                // For now, just call WorldLoop with the remaining chunk budget
                m_world->WorldLoop(deltaTime, m_config.maxChunksPerTick - chunksProcessed);
                break; // WorldLoop handles its own batching
            }
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
        if (m_playerState.loadedChunks.find(chunkPos) != m_playerState.loadedChunks.end()) {
            Log::Debug("Chunk (%d, %d) already loaded by client, skipping send", chunkPos.x, chunkPos.z);
            return;
        }
        
        // Create and send ChunkDataS2CPacket
        Network::ChunkDataS2CPacket packet;
        packet.chunkX = chunkPos.x;
        packet.chunkZ = chunkPos.z;
        packet.groundUpContinuous = true;
        // TODO: Properly serialize chunk data into sections
        SendChunkDataS2CPacket(std::move(packet));
        
        // Update send state
        ChunkSendState& sendState = m_chunkSendStates[chunkPos];
        sendState.chunkPos = chunkPos;
        sendState.sent = true;
        sendState.needsResend = false;
        sendState.sendTime = std::chrono::steady_clock::now();
        
        // Mark chunk as loaded by the client
        m_playerState.loadedChunks.insert(chunkPos);
        
        m_stats.chunksSent.fetch_add(1, std::memory_order_relaxed);
        Log::Debug("Sent chunk (%d, %d) to client", chunkPos.x, chunkPos.z);
    }
    
    void IntegratedServer::ProcessAsyncChunkResults() {
        // Only process if async chunk loading is enabled
        if (!m_config.enableAsyncChunkLoading) {
            return;
        }
        
        // Get the chunk generation result queue from ServerWorkerPool
        auto& resultQueue = Threading::ServerWorkerPool::GetChunkGenResultQueue();
        
        // Process up to a limited number of results per tick to avoid stalling
        const int maxResultsPerTick = 10;
        int resultsProcessed = 0;
        
        Network::ChunkGenResult result;
        while (resultsProcessed < maxResultsPerTick && resultQueue.try_pop(result)) {
            // Check if chunk is still in pending list
            if (m_pendingChunkLoads.find(result.position) != m_pendingChunkLoads.end()) {
                if (result.success && result.chunk) {
                    // Send the chunk to the client
                    SendChunkToClient(result.position, result.chunk);
                    
                    // The chunk is already stored in the world by the worker thread
                    // No need to store it again
                    
                    Log::Debug("Processed async chunk load for (%d, %d) - success", 
                             result.position.x, result.position.z);
                } else {
                    Log::Warning("Async chunk load failed for (%d, %d): %s", 
                               result.position.x, result.position.z, 
                               result.errorMessage.c_str());
                }
                
                // Remove from pending list
                m_pendingChunkLoads.erase(result.position);
            }
            
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
            m_playerState.position
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

    void IntegratedServer::ProcessPlayerMove(const Network::PlayerMoveC2SPacket& packet) {
        if (!ValidatePlayerMove(packet)) {
            Log::Warning("Invalid player move received");
            return;
        }
        
        UpdatePlayerState(packet);
        m_stats.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    }
    
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
        
        // Create player session
        if (m_sessionManager) {
            uint32_t playerId = 1;  // Single player ID for integrated server
            m_sessionManager->OnPlayerJoin(
                playerId,
                connection->GetConnectionId(),
                "Player"  // Default player name
            );
            
            Log::Info("[IntegratedServer] Player session created and initialized");
        } else {
            // Fallback to old system if session manager not available
            Log::Warning("[IntegratedServer] Session manager not available, using legacy chunk sending");
            
            // Get spawn position (player's initial position)
            glm::vec3 spawnPos = m_playerState.position;
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
            
            // Clear loaded chunks - they will be added as chunks are actually sent
            m_playerState.loadedChunks.clear();
        }  // End of else block for legacy chunk sending
    }

    bool IntegratedServer::ValidatePlayerMove(const Network::PlayerMoveC2SPacket& packet) const {
        // Basic validation - check for reasonable movement speed
        float distance = glm::distance(packet.position, m_playerState.position);
        auto timeDiff = packet.timestamp - 
                       std::chrono::time_point_cast<std::chrono::steady_clock::duration>(
                           std::chrono::steady_clock::time_point{} + 
                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               m_playerState.lastUpdateTime.time_since_epoch()));
        
        float timeDiffSeconds = std::chrono::duration<float>(timeDiff).count();
        if (timeDiffSeconds > 0.0f) {
            float speed = distance / timeDiffSeconds;
            if (speed > 100.0f) { // Max reasonable speed
                return false;
            }
        }
        
        return true;
    }

    void IntegratedServer::UpdatePlayerChunkPosition(const glm::vec3& newPosition) {
        auto newChunk = Game::Math::WorldCoordinates::WorldToChunkPos(
            static_cast<int>(newPosition.x), 
            static_cast<int>(newPosition.z)
        );
        
        if (newChunk.x != m_playerState.currentChunk.x || newChunk.z != m_playerState.currentChunk.z) {
            m_playerState.currentChunk = newChunk;
            Log::Debug("Player moved to chunk (%d, %d)", newChunk.x, newChunk.z);
            
            // Chunk loading is now handled by World::WorldLoop()
        }
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
        auto playerChunk = Game::Math::WorldCoordinates::WorldToChunkPos(
            static_cast<int>(m_playerState.position.x),
            static_cast<int>(m_playerState.position.z)
        );
        
        int dx = std::abs(chunkPos.x - playerChunk.x);
        int dz = std::abs(chunkPos.z - playerChunk.z);
        
        // Chebyshev distance = max(abs(dx), abs(dz))
        // Convert to float for compatibility with existing code
        return static_cast<float>(std::max(dx, dz));
    }

    std::vector<Game::Math::ChunkPos> IntegratedServer::GetRequiredChunks() const {
        std::vector<Game::Math::ChunkPos> chunks;
        
        auto playerChunk = m_playerState.currentChunk;
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
        Log::Info("Server State: TPS=%.1f, TickTime=%.2fms, LoadedChunks=%zu, PendingLoads=%zu",
                 m_stats.averageTPS.load(), m_stats.averageTickTime.load(),
                 m_playerState.loadedChunks.size(), m_pendingChunkLoads.size());
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
        
        Log::Info("Session management system initialized successfully");
    }

    void IntegratedServer::CleanupSessionSystem() {
        Log::Info("Cleaning up session management system...");
        
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
            m_watchIndex->Clear();
            m_watchIndex.reset();
        }
        
        if (m_ticketManager) {
            m_ticketManager->Clear();
            m_ticketManager.reset();
        }
        
        Log::Info("Session management system cleaned up");
    }

} // namespace Server