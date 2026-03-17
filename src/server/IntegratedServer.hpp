// File: src/server/IntegratedServer.hpp
#pragma once

#include "common/network/PacketTypes.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/level/World.hpp"
#include "common/network/AsioInclude.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <unordered_map>

namespace Game {
    class ClientPlayer;
}

namespace Server {

    // Forward declarations
    class ChunkTicketManager;
    class ChunkWatchIndex;
    class ChunkStatusManager;
    class SendScheduler;
    class PlayerSessionManager;
    class ServerPlayer;
    class PlayerSession;
    class SectionChangeAccumulator;
    class ChunkDeltaBroadcaster;
    
    // Server thread tracking
    extern std::thread::id g_serverThreadId;

    // Server configuration
    struct IntegratedServerConfig {
        int tickRate = 20;                      // Server ticks per second (20 TPS like Minecraft)
        int maxChunksPerTick = 32;             // Max chunks to process per tick (increased from 5)
        float chunkProcessBudgetMs = 2.0f;     // Time budget for chunk processing per tick
        int defaultViewDistance = 8;           // Default view distance in chunks (Minecraft-like)
        int serverViewDistance = 32;           // Server's max view distance cap (clients clamped to this)
        bool enableAsyncChunkLoading = true;   // Use ServerWorkerPool for chunk loading
        bool enableChunkCaching = true;        // Keep recently used chunks in memory
        std::string minecraftWorldPath;        // Optional Minecraft world to load (empty by default)
        bool useLocalSaveDirectory = true;     // Automatically use local save directory if available (temporary feature)
    };


    // Integrated server class (mirrors MinecraftServer + IntegratedServer from Minecraft)
    class IntegratedServer {
    public:
        explicit IntegratedServer(const IntegratedServerConfig& config = IntegratedServerConfig{});
        ~IntegratedServer();

        // Non-copyable, non-movable
        IntegratedServer(const IntegratedServer&) = delete;
        IntegratedServer& operator=(const IntegratedServer&) = delete;

        // ========================================================================
        // SERVER LIFECYCLE
        // ========================================================================

        // Initialize server (creates world internally)
        bool Initialize();

        // Start server thread (20 TPS loop)
        bool Start();

        // Stop server gracefully
        void Stop();

        // Check if server is running
        bool IsRunning() const { return m_running.load(); }

        // Shutdown and cleanup
        void Shutdown();

        // ========================================================================
        // WORLD MANAGEMENT
        // ========================================================================

        // Get the server-owned world instance
        Game::World* GetWorld() const { return m_world.get(); }
        
        // Get the change accumulator for block updates
        SectionChangeAccumulator* GetChangeAccumulator() const { return m_changeAccumulator.get(); }

        // ========================================================================
        // PLAYER MANAGEMENT
        // ========================================================================

        // Set player (for integrated server)
        void SetPlayer(Game::ClientPlayer* player);
        
        // Get player position/chunk from active session (single source of truth)
        Game::Math::ChunkPos GetPlayerChunkPosition() const;
        glm::vec3 GetPlayerPosition() const;

        // Get player session for network/view management
        // NOTE: Delegates to SessionManager now instead of using m_playerSession
        std::shared_ptr<PlayerSession> GetPlayerSession() const;

        // Get session manager for accessing player sessions
        PlayerSessionManager* GetSessionManager() const { return m_sessionManager.get(); }

        // Get status manager for chunk generation tracking
        ChunkStatusManager* GetStatusManager() const { return m_statusManager.get(); }

        // Process watch set changes: request loading for new chunks, unload for removed
        void ProcessWatchSetChanges();

        // Unload chunks that no player is watching (periodic cleanup)
        void UnloadUnwatchedChunks();
        
        // Send block change packets to client
        void SendBlockChangeS2CPacket(const Network::BlockChangeS2CPacket& packet);
        void SendSectionBlocksUpdateS2CPacket(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet);

        // ========================================================================
        // PACKET PROCESSING (Called by NetworkServer)
        // ========================================================================
        
        // Process incoming block action packet
        void ProcessBlockAction(const Network::BlockActionC2SPacket& packet);
        
        // Process incoming chat message
        void ProcessChatMessage(const Network::ChatMessageC2SPacket& packet);
        
        // Called when a player successfully logs in and needs initial chunks
        void OnPlayerJoined(std::shared_ptr<class ServerConnection> connection);

        // Called when a player disconnects (TCP close)
        void OnPlayerDisconnected(std::shared_ptr<class ServerConnection> connection);

        // Called when server receives client settings (render distance, etc.)
        void OnClientSettingsReceived(uint32_t connectionId, int requestedViewDistance);

        // Send the effective view distance to the client
        void SendSetChunkCacheRadius(uint32_t connectionId, int viewDistance);

        // ========================================================================
        // CONFIGURATION
        // ========================================================================

        void SetConfig(const IntegratedServerConfig& config) { m_config = config; }
        const IntegratedServerConfig& GetConfig() const { return m_config; }

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ServerStats {
            std::atomic<uint64_t> ticksProcessed{0};
            std::atomic<uint64_t> chunksLoaded{0};
            std::atomic<uint64_t> chunksSent{0};
            std::atomic<uint64_t> blockChangesProcessed{0};
            std::atomic<uint64_t> packetsReceived{0};
            std::atomic<uint64_t> packetsSent{0};
            std::atomic<float> averageTickTime{0.0f};
            std::atomic<float> averageTPS{20.0f};

            void Reset() {
                ticksProcessed = chunksLoaded = chunksSent = blockChangesProcessed = 0;
                packetsReceived = packetsSent = 0;
                averageTickTime = 0.0f;
                averageTPS = 20.0f;
            }
        };

        const ServerStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }
        void LogStats() const;

        // Chunk streaming metrics
        size_t GetPendingChunkLoadCount() const { return m_pendingChunkLoads.size(); }

    private:
        // Configuration
        IntegratedServerConfig m_config;

        // Server-owned world instance
        std::unique_ptr<Game::World> m_world;
        
        // New session management system
        std::unique_ptr<ChunkTicketManager> m_ticketManager;
        std::unique_ptr<ChunkWatchIndex> m_watchIndex;
        std::unique_ptr<ChunkStatusManager> m_statusManager;
        std::unique_ptr<SendScheduler> m_sendScheduler;
        std::unique_ptr<PlayerSessionManager> m_sessionManager;
        
        // Block change accumulation and broadcasting
        std::unique_ptr<SectionChangeAccumulator> m_changeAccumulator;
        std::unique_ptr<ChunkDeltaBroadcaster> m_deltaBroadcaster;

        // Player reference (for integrated server)
        Game::ClientPlayer* m_player = nullptr;

        // Thread management
        std::unique_ptr<std::thread> m_serverThread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_shouldStop{false};

        // New player architecture
        std::unique_ptr<ServerPlayer> m_serverPlayer;     // Host player (ID 1)
        std::unordered_map<uint32_t, std::unique_ptr<ServerPlayer>> m_remotePlayers; // Remote players by ID
        // NOTE: PlayerSession is now managed by PlayerSessionManager, not stored here

        // Chunk management
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_pendingChunkLoads;

        // Statistics
        ServerStats m_stats;

        // Timing
        std::chrono::steady_clock::time_point m_lastTickTime;
        std::chrono::steady_clock::time_point m_lastTickStartTime;  // For accurate TPS calculation
        std::chrono::duration<float> m_tickDuration{1.0f / 20.0f}; // 50ms per tick

        // ========================================================================
        // SERVER THREAD MAIN LOOP
        // ========================================================================

        // Main server loop (runs at 20 TPS)
        void ServerLoop();

        // Single server tick
        void ServerTick();
        
        // Initialize the new session management system
        void InitializeSessionSystem();
        
        // Cleanup the session management system
        void CleanupSessionSystem();

        // ========================================================================
        // DISCIPLINED QUEUE DRAINING (ALWAYS FIRST IN TICK)
        // ========================================================================

        // Process all Client→Server packets
        void ProcessClientToServerPackets();


        // ========================================================================
        // CHUNK MANAGEMENT
        // ========================================================================

        // Request chunk loading (either sync or async via ServerWorkerPool)
        void RequestChunkLoad(Game::Math::ChunkPos chunkPos, int priority = 0);

        // Process async chunk load results from ServerWorkerPool
        void ProcessAsyncChunkResults();

        // ========================================================================
        // BLOCK CHANGE PROCESSING (Private implementation details)
        // ========================================================================

        // Validate block action
        bool ValidateBlockAction(const Network::BlockActionC2SPacket& packet) const;

        // Apply block change and notify client
        void ApplyBlockChange(int worldX, int worldY, int worldZ, Game::BlockID blockId);

        // ========================================================================
        // PLAYER UPDATE PROCESSING (Private implementation details)
        // ========================================================================

        // Update view distance watchers
        void UpdateViewDistanceWatchers();

        // ========================================================================
        // PACKET SENDING
        // ========================================================================

        // Send BlockChangeS2CPacket to client
        void SendPacketToClient(Network::BlockChangeS2CPacket&& packet);

        // Send MultiBlockChangeS2CPacket to client
        void SendPacketToClient(Network::MultiBlockChangeS2CPacket&& packet);

        // ========================================================================
        // UTILITY METHODS
        // ========================================================================

        // Calculate distance from player to chunk
        float CalculateChunkDistance(Game::Math::ChunkPos chunkPos) const;

        // Check if chunk is within send radius
        bool IsChunkInSendRadius(Game::Math::ChunkPos chunkPos) const;

        // Get chunks that should be loaded around player
        std::vector<Game::Math::ChunkPos> GetRequiredChunks() const;

        // Update statistics
        void UpdateStatistics(float tickExecutionTime, float timeBetweenTicks);

        // Log server state for debugging
        void LogServerState() const;
        
        // Network server
        std::unique_ptr<net::io_context> m_ioContext;
        std::unique_ptr<class NetworkServer> m_networkServer;
        
        // Dedicated I/O thread and work guard (Minecraft-style Netty pattern)
        using WorkGuard = net::executor_work_guard<net::io_context::executor_type>;
        std::unique_ptr<WorkGuard> m_ioWorkGuard;
        std::unique_ptr<std::thread> m_networkThread;
        
    public:
        // Get network server for direct access
        NetworkServer* GetNetworkServer() const { return m_networkServer.get(); }
    };

    // ========================================================================
    // GLOBAL ACCESS
    // ========================================================================

    // Global integrated server instance
    extern std::unique_ptr<IntegratedServer> g_integratedServer;

    // Convenience functions
    void InitializeIntegratedServer(const IntegratedServerConfig& config = IntegratedServerConfig{});
    void StartIntegratedServer();
    void StopIntegratedServer();
    void ShutdownIntegratedServer();

    // Server state queries
    bool IsIntegratedServerRunning();
    const IntegratedServer::ServerStats& GetIntegratedServerStats();

} // namespace Server