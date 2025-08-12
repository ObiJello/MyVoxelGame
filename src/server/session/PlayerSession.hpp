// File: src/server/session/PlayerSession.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include <glm/glm.hpp>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <queue>
#include <memory>
#include <chrono>
#include <mutex>
#include <atomic>

namespace Server {

    // Forward declarations
    class ServerConnection;
    class ChunkTicketManager;
    class ChunkWatchIndex;
    class ChunkStatusManager;
    class SendScheduler;

    // Ring-based priority queue for chunk streaming
    template<typename T>
    class RingPriorityQueue {
    public:
        explicit RingPriorityQueue(size_t maxRings = 32) : m_rings(maxRings) {}
        
        void Push(const T& item, size_t ring) {
            if (ring < m_rings.size()) {
                m_rings[ring].push_back(item);
                m_totalSize++;
            }
        }
        
        bool Pop(T& item) {
            for (auto& ring : m_rings) {
                if (!ring.empty()) {
                    item = ring.front();
                    ring.erase(ring.begin());
                    m_totalSize--;
                    return true;
                }
            }
            return false;
        }
        
        bool Empty() const { return m_totalSize == 0; }
        size_t Size() const { return m_totalSize; }
        
        void Clear() {
            for (auto& ring : m_rings) {
                ring.clear();
            }
            m_totalSize = 0;
        }
        
    private:
        std::vector<std::vector<T>> m_rings;
        size_t m_totalSize = 0;
    };

    // Coalesced block changes for a chunk section
    struct SectionDiffs {
        Game::Math::ChunkPos chunkPos;
        int sectionIndex;
        std::unordered_map<uint32_t, Game::BlockID> changes; // localIndex -> blockId
        std::chrono::steady_clock::time_point lastUpdate;
        
        uint32_t MakeIndex(uint8_t x, uint8_t y, uint8_t z) const {
            return (y << 8) | (z << 4) | x;
        }
        
        void AddChange(uint8_t x, uint8_t y, uint8_t z, Game::BlockID blockId) {
            changes[MakeIndex(x, y, z)] = blockId;
            lastUpdate = std::chrono::steady_clock::now();
        }
    };

    // Player session managing watch sets and streaming
    class PlayerSession {
    public:
        // Configuration
        struct Config {
            int simulationDistance = 8;    // Chunks kept loaded around player
            int viewDistance = 8;          // Chunks sent to client (≤ simulationDistance)
            int maxChunksPerTick = 12;     // Max chunks to send per tick
            int maxBytesPerTick = 1048576; // 1MB per tick max
            int maxDiffsPerTick = 100;     // Max block changes per tick
            bool compressPackets = true;   // Enable compression
            int compressionLevel = 3;      // zlib compression level (1-9)
        };

        // Session state
        enum class State {
            CONNECTING,      // Initial handshake
            AUTHENTICATING,  // Login process
            JOINING,         // Sending join game packet
            PLAYING,         // Normal gameplay
            RESPAWNING,      // Respawn in progress
            DISCONNECTING    // Cleanup in progress
        };

        PlayerSession(uint32_t playerId, uint32_t connectionId);
        ~PlayerSession();

        // === LIFECYCLE ===

        // Initialize session after successful login
        void Initialize(const Config& config, int dimensionId, const glm::vec3& spawnPos);
        
        // Process one server tick
        void Tick(int64_t serverTick);
        
        // Cleanup on disconnect
        void Cleanup();

        // === PLAYER STATE ===

        // Update player position (from client packets)
        void UpdatePosition(const glm::vec3& position, const glm::vec2& rotation);
        
        // Update player's chunk position
        void UpdateChunkPosition(Game::Math::ChunkPos newChunk);
        
        // Change dimension
        void ChangeDimension(int newDimensionId, const glm::vec3& targetPos);
        
        // Respawn player
        void Respawn(const glm::vec3& spawnPos);

        // === VIEW CONFIGURATION ===

        // Update view distance (client request or server override)
        void SetViewDistance(int distance);
        
        // Update simulation distance (server config)
        void SetSimulationDistance(int distance);

        // === WATCH SET MANAGEMENT ===

        // Compute and apply watch set changes
        void UpdateWatchSet();
        
        // Check if chunk is in watch set
        bool IsWatching(Game::Math::ChunkPos chunk) const;
        
        // Check if chunk has been sent
        bool HasSentChunk(Game::Math::ChunkPos chunk) const;

        // === CHUNK STREAMING ===

        // Queue chunk for sending
        void QueueChunkSend(Game::Math::ChunkPos chunk, int priority = 0);
        
        // Process chunk sends for this tick
        void ProcessChunkSends(ChunkStatusManager* statusMgr, SendScheduler* scheduler);
        
        // Send unload packet for chunk
        void SendChunkUnload(Game::Math::ChunkPos chunk);

        // === BLOCK UPDATES ===

        // Queue block change diff
        void QueueBlockChange(int worldX, int worldY, int worldZ, Game::BlockID newBlock);
        
        // Queue multi-block change for a section
        void QueueSectionChanges(Game::Math::ChunkPos chunk, int section, 
                                const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes);
        
        // Process and send diffs for this tick
        void ProcessDiffs(SendScheduler* scheduler);

        // === PACKET HANDLING ===

        // Handle incoming packets
        void HandlePlayerMove(const Network::PlayerMoveC2SPacket& packet);
        void HandleBlockAction(const Network::BlockActionC2SPacket& packet);
        void HandleKeepAlive(const Network::KeepAliveC2SPacket& packet);
        
        // Track packet acknowledgments
        void OnChunkSendComplete(Game::Math::ChunkPos chunk);
        void OnChunkUnloadComplete(Game::Math::ChunkPos chunk);

        // === STATISTICS ===

        struct Stats {
            // Streaming stats
            size_t chunksInWatch = 0;
            size_t chunksSent = 0;
            size_t chunksInFlight = 0;
            size_t chunksPending = 0;
            
            // Bandwidth stats
            size_t bytesOutThisTick = 0;
            size_t totalBytesOut = 0;
            float averageBytesPerTick = 0;
            
            // Diff stats
            size_t diffsQueued = 0;
            size_t diffsSent = 0;
            size_t diffsDropped = 0;
            
            // Timing stats
            float lastTickTime = 0;
            float averageTickTime = 0;
            
            // Connection stats
            float latency = 0;
            std::chrono::steady_clock::time_point lastKeepAlive;
        };
        
        Stats GetStats() const;
        void ResetStats();

        // === GETTERS ===

        uint32_t GetPlayerId() const { return m_playerId; }
        uint32_t GetConnectionId() const { return m_connectionId; }
        int GetDimensionId() const { return m_dimensionId; }
        State GetState() const { return m_state; }
        
        glm::vec3 GetPosition() const { return m_position; }
        glm::vec2 GetRotation() const { return m_rotation; }
        Game::Math::ChunkPos GetChunkPosition() const { return m_currentChunk; }
        Game::Math::ChunkPos GetAnchorChunk() const { return m_anchorChunk; }
        
        int GetViewDistance() const { return m_viewDistance; }
        int GetSimulationDistance() const { return m_simulationDistance; }

    private:
        // === IDENTIFIERS ===
        uint32_t m_playerId;
        uint32_t m_connectionId;
        int m_dimensionId;
        
        // === STATE ===
        std::atomic<State> m_state{State::CONNECTING};
        Config m_config;
        
        // === POSITION ===
        glm::vec3 m_position{0.0f};
        glm::vec2 m_rotation{0.0f};
        Game::Math::ChunkPos m_currentChunk{0, 0};
        Game::Math::ChunkPos m_anchorChunk{0, 0}; // Center for watch calculations
        Game::Math::ChunkPos m_lastKnownChunk{0, 0};
        
        // === DISTANCES ===
        int m_simulationDistance = 8;
        int m_viewDistance = 8;
        
        // === WATCH SETS ===
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_watchSet;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_sentChunks;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_inflightChunks;
        
        // === STREAMING QUEUES ===
        RingPriorityQueue<Game::Math::ChunkPos> m_pendingAdd;
        std::vector<Game::Math::ChunkPos> m_pendingRemove;
        
        // === DIFF MANAGEMENT ===
        std::unordered_map<Game::Math::ChunkPos, 
                          std::unordered_map<int, SectionDiffs>, 
                          Game::Math::ChunkPosHash> m_pendingDiffs;
        std::queue<std::pair<Game::Math::ChunkPos, int>> m_diffQueue; // (chunk, section) pairs
        
        // === BUDGETS ===
        size_t m_bytesOutThisTick = 0;
        size_t m_chunksOutThisTick = 0;
        size_t m_diffsOutThisTick = 0;
        
        // === STATISTICS ===
        mutable std::mutex m_statsMutex;
        Stats m_stats;
        std::chrono::steady_clock::time_point m_lastTickTime;
        
        // === TIMING ===
        std::chrono::steady_clock::time_point m_lastKeepAliveRx;
        std::chrono::steady_clock::time_point m_lastKeepAliveTx;
        int64_t m_lastServerTick = 0;
        
        // === FLAGS ===
        bool m_isChangingDimension = false;
        bool m_isRespawning = false;
        bool m_needsWatchUpdate = true;
        
        // === INTERNAL METHODS ===
        
        // Watch set computation
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> 
            ComputeWatchSet(Game::Math::ChunkPos anchor, int viewDistance) const;
        
        // Watch delta computation
        void ComputeWatchDeltas(
            const std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>& newWatch,
            std::vector<Game::Math::ChunkPos>& toAdd,
            std::vector<Game::Math::ChunkPos>& toRemove
        ) const;
        
        // Priority calculation
        int CalculateChunkPriority(Game::Math::ChunkPos chunk) const;
        
        // Diff coalescing
        void CoalesceBlockChange(Game::Math::ChunkPos chunk, int section,
                                uint8_t localX, uint8_t localY, uint8_t localZ,
                                Game::BlockID blockId);
        
        // Packet size estimation
        size_t EstimatePacketSize(const Network::ChunkDataS2CPacket& packet) const;
        size_t EstimatePacketSize(const Network::MultiBlockChangeS2CPacket& packet) const;
        
        // Cleanup helpers
        void ClearWatchSets();
        void ClearQueues();
        void ClearDiffs();
    };

} // namespace Server