// File: src/server/IntegratedServer.hpp
#pragma once

#include "common/network/PacketTypes.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/level/World.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <unordered_map>

namespace Game {
    class PlayerController;
}

namespace Server {

    // Forward declarations
    class ChunkTicketManager;
    class ChunkWatchIndex;
    class ChunkStatusManager;
    class SendScheduler;
    class PlayerSessionManager;

    // Server configuration
    struct IntegratedServerConfig {
        int tickRate = 20;                      // Server ticks per second (20 TPS like Minecraft)
        int maxChunksPerTick = 5;              // Max chunks to process per tick
        int maxBlockChangesPerTick = 100;      // Max block changes to process per tick
        float chunkSendRadius = 128.0f;        // Radius for sending chunks to client
        bool enableAsyncChunkLoading = true;   // Use ServerWorkerPool for chunk loading
        bool enableChunkCaching = true;        // Keep recently used chunks in memory
        size_t maxCachedChunks = 1024;         // Max chunks to keep cached
        std::string minecraftWorldPath;        // Optional Minecraft world to load (empty by default)
    };

    // Player state tracking (for multiplayer extension)
    struct ServerPlayerState {
        uint32_t playerId = 0;
        glm::vec3 position{0.0f};
        glm::vec2 rotation{0.0f};
        Game::Math::ChunkPos currentChunk{0, 0};
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> loadedChunks;
        uint32_t lastMoveSequenceNumber = 0;
        std::chrono::steady_clock::time_point lastUpdateTime;
    };

    // Chunk send tracking
    struct ChunkSendState {
        Game::Math::ChunkPos chunkPos{0, 0};  // Initialize to (0,0)
        bool sent = false;
        bool needsResend = false;
        std::chrono::steady_clock::time_point sendTime;
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

        // ========================================================================
        // PLAYER MANAGEMENT
        // ========================================================================

        // Set player controller (for integrated server)
        void SetPlayerController(Game::PlayerController* playerController);
        
        // Update player state from client packets
        void UpdatePlayerState(const Network::PlayerMoveC2SPacket& packet);

        // Get current player state
        const ServerPlayerState& GetPlayerState() const { return m_playerState; }
        
        // Send ChunkDataS2CPacket to client (new Minecraft-compatible format)
        void SendChunkDataS2CPacket(Network::ChunkDataS2CPacket&& packet);

        // ========================================================================
        // PACKET PROCESSING (Called by NetworkServer)
        // ========================================================================
        
        // Process incoming block action packet
        void ProcessBlockAction(const Network::BlockActionC2SPacket& packet);
        
        // Process incoming player move packet
        void ProcessPlayerMove(const Network::PlayerMoveC2SPacket& packet);
        
        // Process incoming chat message
        void ProcessChatMessage(const Network::ChatMessageC2SPacket& packet);
        
        // Called when a player successfully logs in and needs initial chunks
        void OnPlayerJoined(std::shared_ptr<class ServerConnection> connection);

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

        // Player controller reference (for integrated server)
        Game::PlayerController* m_playerController = nullptr;

        // Thread management
        std::unique_ptr<std::thread> m_serverThread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_shouldStop{false};

        // Player state
        ServerPlayerState m_playerState;

        // Chunk management
        std::unordered_map<Game::Math::ChunkPos, ChunkSendState, Game::Math::ChunkPosHash> m_chunkSendStates;
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

        // Check if chunk should be sent to client
        bool ShouldSendChunk(Game::Math::ChunkPos chunkPos) const;

        // Send chunk data packet to client
        void SendChunkToClient(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk);

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

        // Validate player movement
        bool ValidatePlayerMove(const Network::PlayerMoveC2SPacket& packet) const;

        // Update player chunk position
        void UpdatePlayerChunkPosition(const glm::vec3& newPosition);

        // ========================================================================
        // PACKET SENDING
        // ========================================================================

        // Send ServerChunkDataPacket to client
        void SendPacketToClient(Network::ServerChunkDataPacket&& packet);

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
        std::unique_ptr<boost::asio::io_context> m_ioContext;
        std::unique_ptr<class NetworkServer> m_networkServer;
        
        // Dedicated I/O thread and work guard (Minecraft-style Netty pattern)
        using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
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