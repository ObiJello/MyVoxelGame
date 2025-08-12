// File: src/server/session/PlayerSessionManager.hpp
#pragma once

#include "PlayerSession.hpp"
#include "common/world/math/WorldMath.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>

namespace Server {

    // Forward declarations
    class ChunkTicketManager;
    class ChunkWatchIndex;
    class ChunkStatusManager;
    class SendScheduler;
    class ServerConnection;

    // Manages all player sessions on the server
    class PlayerSessionManager {
    public:
        // Configuration
        struct Config {
            // Default player settings
            int defaultSimulationDistance = 8;
            int defaultViewDistance = 8;
            int maxViewDistance = 32;
            
            // Spawn settings
            glm::vec3 worldSpawn{0.0f, 64.0f, 0.0f};
            int spawnChunkRadius = 2;  // Keep spawn chunks always loaded
            bool spawnProtection = true;
            int spawnProtectionRadius = 16;  // blocks
            
            // Performance settings
            int maxPlayersPerTick = 100;  // Max players to process per tick
            int maxChunksPerPlayerPerTick = 12;
            int maxBytesPerPlayerPerTick = 1048576;  // 1MB per player per tick
            int maxDiffBytesPerPlayerPerTick = 524288;  // 512KB for block changes per player
            int maxGlobalChunksPerTick = 100;
            size_t maxGlobalBytesPerTick = 10485760;  // 10MB
            
            // Session settings
            int sessionTimeoutSeconds = 30;
            bool kickOnTimeout = true;
        };

        PlayerSessionManager();
        ~PlayerSessionManager();

        // === INITIALIZATION ===

        // Initialize with dependencies
        void Initialize(
            const Config& config,
            ChunkTicketManager* ticketMgr,
            ChunkWatchIndex* watchIndex,
            ChunkStatusManager* statusMgr,
            SendScheduler* scheduler
        );
        
        // Shutdown and cleanup all sessions
        void Shutdown();

        // === SESSION MANAGEMENT ===

        // Create a new session for a player
        std::shared_ptr<PlayerSession> CreateSession(
            uint32_t playerId,
            uint32_t connectionId,
            const std::string& playerName
        );
        
        // Get session by player ID
        std::shared_ptr<PlayerSession> GetSession(uint32_t playerId) const;
        
        // Get session by connection ID
        std::shared_ptr<PlayerSession> GetSessionByConnection(uint32_t connectionId) const;
        
        // Remove a session (on disconnect)
        void RemoveSession(uint32_t playerId);
        
        // Get all active sessions
        std::vector<std::shared_ptr<PlayerSession>> GetAllSessions() const;
        
        // Get session count
        size_t GetSessionCount() const;

        // === PLAYER LIFECYCLE ===

        // Handle player join (after successful login)
        void OnPlayerJoin(
            uint32_t playerId,
            uint32_t connectionId,
            const std::string& playerName
        );
        
        // Handle player leave (disconnect or kick)
        void OnPlayerLeave(uint32_t playerId, const std::string& reason);
        
        // Handle player death
        void OnPlayerDeath(uint32_t playerId);
        
        // Handle player respawn
        void OnPlayerRespawn(uint32_t playerId);

        // === TICK PROCESSING ===

        // Process all sessions for one server tick
        void Tick(int64_t serverTick);
        
        // Process chunk sends for all players
        void ProcessChunkStreaming();
        
        // Process block diffs for all players
        void ProcessBlockDiffs();
        
        // Process keep-alive and timeouts
        void ProcessKeepAlive();

        // === WORLD UPDATES ===

        // Notify all watching players of a block change
        void BroadcastBlockChange(
            int worldX, int worldY, int worldZ,
            Game::BlockID newBlock
        );
        
        // Notify all watching players of section changes
        void BroadcastSectionChanges(
            Game::Math::ChunkPos chunk,
            int section,
            const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes
        );
        
        // Notify all watching players of chunk update
        void BroadcastChunkUpdate(Game::Math::ChunkPos chunk);
        
        // Notify all watching players of light update
        void BroadcastLightUpdate(
            Game::Math::ChunkPos chunk,
            const std::vector<uint8_t>& sectionMask
        );

        // === SPAWN MANAGEMENT ===

        // Set world spawn point
        void SetWorldSpawn(const glm::vec3& spawnPos);
        
        // Get world spawn point
        glm::vec3 GetWorldSpawn() const;
        
        // Set player spawn point
        void SetPlayerSpawn(uint32_t playerId, const glm::vec3& spawnPos);
        
        // Get player spawn point (or world spawn if not set)
        glm::vec3 GetPlayerSpawn(uint32_t playerId) const;

        // === CONFIGURATION ===

        // Update configuration
        void SetConfig(const Config& config);
        
        // Get current configuration
        const Config& GetConfig() const;
        
        // Set max view distance for all players
        void SetMaxViewDistance(int distance);
        
        // Set default simulation distance
        void SetDefaultSimulationDistance(int distance);

        // === STATISTICS ===

        struct Stats {
            size_t totalSessions = 0;
            size_t activeSessions = 0;
            size_t totalChunksStreamed = 0;
            size_t totalBytesStreamed = 0;
            size_t chunksPerTick = 0;
            size_t bytesPerTick = 0;
            float averageLatency = 0;
            float averageChunksPerPlayer = 0;
            size_t playersTimedOut = 0;
        };
        
        Stats GetStats() const;
        void ResetStats();

        // === CALLBACKS ===

        // Set callback for when a player fully joins
        using JoinCallback = std::function<void(uint32_t playerId)>;
        void SetJoinCallback(JoinCallback callback);
        
        // Set callback for when a player leaves
        using LeaveCallback = std::function<void(uint32_t playerId, const std::string& reason)>;
        void SetLeaveCallback(LeaveCallback callback);

    private:
        // Configuration
        Config m_config;
        
        // Dependencies
        ChunkTicketManager* m_ticketManager = nullptr;
        ChunkWatchIndex* m_watchIndex = nullptr;
        ChunkStatusManager* m_statusManager = nullptr;
        SendScheduler* m_sendScheduler = nullptr;
        
        // Player sessions
        std::unordered_map<uint32_t, std::shared_ptr<PlayerSession>> m_sessions;
        std::unordered_map<uint32_t, uint32_t> m_connectionToPlayer;  // connectionId -> playerId
        std::unordered_map<uint32_t, std::string> m_playerNames;
        
        // Player spawns
        std::unordered_map<uint32_t, glm::vec3> m_playerSpawns;
        
        // Thread safety
        mutable std::mutex m_sessionMutex;
        mutable std::mutex m_statsMutex;
        
        // Statistics
        Stats m_stats;
        
        // Callbacks
        JoinCallback m_joinCallback;
        LeaveCallback m_leaveCallback;
        
        // State
        bool m_initialized = false;
        int64_t m_currentTick = 0;
        
        // === INTERNAL METHODS ===
        
        // Update tickets for a player's simulation distance
        void UpdatePlayerTickets(
            uint32_t playerId,
            Game::Math::ChunkPos oldAnchor,
            Game::Math::ChunkPos newAnchor,
            int simulationDistance
        );
        
        // Update watch index for a player
        void UpdatePlayerWatchers(
            uint32_t playerId,
            const std::vector<Game::Math::ChunkPos>& toAdd,
            const std::vector<Game::Math::ChunkPos>& toRemove
        );
        
        // Get players watching a chunk
        std::vector<uint32_t> GetChunkWatchers(Game::Math::ChunkPos chunk) const;
        
        // Process individual session tick
        void ProcessSessionTick(std::shared_ptr<PlayerSession> session);
        
        // Check session timeout
        bool IsSessionTimedOut(std::shared_ptr<PlayerSession> session) const;
        
        // Initialize spawn chunks
        void InitializeSpawnChunks();
        
        // Cleanup session resources
        void CleanupSession(uint32_t playerId);
    };

} // namespace Server