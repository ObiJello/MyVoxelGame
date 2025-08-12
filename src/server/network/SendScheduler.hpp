// File: src/server/network/SendScheduler.hpp
#pragma once

#include "common/network/PacketTypes.hpp"
#include "common/world/math/WorldMath.hpp"
#include <memory>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>

namespace Server {

    // Forward declarations
    class ServerConnection;

    // Packet priority levels
    enum class PacketPriority {
        CRITICAL = 0,    // System packets (disconnect, keepalive)
        HIGH = 1,        // Player actions, block changes
        NORMAL = 2,      // Chunk data, entity updates
        LOW = 3,         // Ambient sounds, particles
        BULK = 4         // Large chunk transfers
    };

    // Queued packet with metadata
    struct QueuedPacket {
        PacketPriority priority;
        size_t estimatedSize;
        std::chrono::steady_clock::time_point queueTime;
        std::function<void()> sendCallback;
        std::function<void()> completionCallback;
        
        // For priority queue ordering (lower priority value = higher priority)
        bool operator<(const QueuedPacket& other) const {
            if (priority != other.priority) {
                return priority > other.priority;  // Inverted for min-heap behavior
            }
            return queueTime > other.queueTime;  // Older packets first
        }
    };

    // Per-connection send state
    struct ConnectionSendState {
        uint32_t connectionId;
        std::priority_queue<QueuedPacket> packetQueue;
        size_t outboxBytes = 0;
        size_t maxOutboxBytes = 4194304;  // 4MB default
        size_t bytesSentThisTick = 0;
        size_t packetsSentThisTick = 0;
        std::chrono::steady_clock::time_point lastSendTime;
        bool isBackpressured = false;
        float bandwidth = 0;  // bytes/sec
        
        // Statistics
        size_t totalBytesSent = 0;
        size_t totalPacketsSent = 0;
        size_t packetsDropped = 0;
    };

    // Manages packet sending with back-pressure control
    class SendScheduler {
    public:
        // Configuration
        struct Config {
            size_t defaultMaxOutboxBytes = 4194304;     // 4MB per connection
            size_t globalMaxBytesPerTick = 10485760;    // 10MB total per tick
            size_t maxPacketsPerConnectionPerTick = 100;
            size_t maxGlobalPacketsPerTick = 1000;
            bool enableCompression = true;
            int compressionThreshold = 256;  // Min size to compress
            bool dropOldPackets = true;      // Drop old low-priority packets
            std::chrono::seconds packetTimeout{5};  // Max queue time
        };

        SendScheduler();
        ~SendScheduler();

        // === INITIALIZATION ===

        // Initialize with configuration
        void Initialize(const Config& config);
        
        // Shutdown scheduler
        void Shutdown();

        // === CONNECTION MANAGEMENT ===

        // Register a connection
        void RegisterConnection(uint32_t connectionId, std::shared_ptr<ServerConnection> connection);
        
        // Unregister a connection
        void UnregisterConnection(uint32_t connectionId);
        
        // Set per-connection limits
        void SetConnectionLimits(uint32_t connectionId, size_t maxOutboxBytes);
        
        // Get connection state
        ConnectionSendState* GetConnectionState(uint32_t connectionId);

        // === PACKET ENQUEUEING ===

        // Enqueue a chunk data packet
        bool EnqueueChunkData(
            uint32_t connectionId,
            const Network::ChunkDataS2CPacket& packet,
            std::function<void()> onComplete = nullptr
        );
        
        // Enqueue a chunk unload packet
        bool EnqueueChunkUnload(
            uint32_t connectionId,
            const Network::UnloadChunkS2CPacket& packet
        );
        
        // Enqueue a block change packet
        bool EnqueueBlockChange(
            uint32_t connectionId,
            const Network::BlockChangeS2CPacket& packet
        );
        
        // Enqueue a multi-block change packet
        bool EnqueueMultiBlockChange(
            uint32_t connectionId,
            const Network::MultiBlockChangeS2CPacket& packet
        );
        
        // Generic packet enqueue
        template<typename PacketType>
        bool EnqueuePacket(
            uint32_t connectionId,
            const PacketType& packet,
            PacketPriority priority,
            size_t estimatedSize,
            std::function<void()> onComplete = nullptr
        );

        // === SENDING ===

        // Process send queues for all connections (called each tick)
        void ProcessSendQueues();
        
        // Flush packets for a specific connection
        void FlushConnection(uint32_t connectionId);
        
        // Force send critical packets
        void SendCriticalPackets();

        // === BACK-PRESSURE ===

        // Check if connection is back-pressured
        bool IsBackpressured(uint32_t connectionId) const;
        
        // Update outbox size (called by I/O thread)
        void UpdateOutboxSize(uint32_t connectionId, size_t newSize);
        
        // Handle write completion (called by I/O thread)
        void OnWriteComplete(uint32_t connectionId, size_t bytesWritten);

        // === STATISTICS ===

        struct Stats {
            size_t totalConnections = 0;
            size_t backpressuredConnections = 0;
            size_t totalQueuedPackets = 0;
            size_t totalBytesSent = 0;
            size_t totalPacketsSent = 0;
            size_t totalPacketsDropped = 0;
            float averageBandwidth = 0;
            size_t peakOutboxSize = 0;
        };
        
        Stats GetStats() const;
        void ResetStats();
        
        // Get per-connection stats
        ConnectionSendState GetConnectionStats(uint32_t connectionId) const;

        // === CONFIGURATION ===

        void SetConfig(const Config& config);
        const Config& GetConfig() const;

    private:
        // Configuration
        Config m_config;
        
        // Connection states
        std::unordered_map<uint32_t, ConnectionSendState> m_connectionStates;
        std::unordered_map<uint32_t, std::shared_ptr<ServerConnection>> m_connections;
        mutable std::mutex m_connectionMutex;
        
        // Global rate limiting
        std::atomic<size_t> m_globalBytesSentThisTick{0};
        std::atomic<size_t> m_globalPacketsSentThisTick{0};
        
        // Statistics
        mutable std::mutex m_statsMutex;
        Stats m_stats;
        
        // State
        bool m_initialized = false;
        std::chrono::steady_clock::time_point m_lastTickTime;
        
        // === INTERNAL METHODS ===
        
        // Process single connection queue
        void ProcessConnectionQueue(
            uint32_t connectionId,
            ConnectionSendState& state,
            size_t& globalBytesRemaining,
            size_t& globalPacketsRemaining
        );
        
        // Send packet to connection
        bool SendPacketToConnection(
            uint32_t connectionId,
            const QueuedPacket& packet
        );
        
        // Drop old packets from queue
        void DropOldPackets(ConnectionSendState& state);
        
        // Calculate packet size
        template<typename PacketType>
        size_t CalculatePacketSize(const PacketType& packet);
        
        // Compress packet data
        std::vector<uint8_t> CompressPacket(const std::vector<uint8_t>& data);
        
        // Update bandwidth calculation
        void UpdateBandwidth(ConnectionSendState& state, size_t bytesSent);
    };

    // Template implementation
    template<typename PacketType>
    bool SendScheduler::EnqueuePacket(
        uint32_t connectionId,
        const PacketType& packet,
        PacketPriority priority,
        size_t estimatedSize,
        std::function<void()> onComplete
    ) {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        auto it = m_connectionStates.find(connectionId);
        if (it == m_connectionStates.end()) {
            return false;
        }
        
        auto& state = it->second;
        
        // Check back-pressure
        if (state.isBackpressured && priority > PacketPriority::HIGH) {
            m_stats.totalPacketsDropped++;
            state.packetsDropped++;
            return false;
        }
        
        // Check queue size limits
        if (state.packetQueue.size() > 1000 && priority >= PacketPriority::LOW) {
            m_stats.totalPacketsDropped++;
            state.packetsDropped++;
            return false;
        }
        
        // Create queued packet
        QueuedPacket queued;
        queued.priority = priority;
        queued.estimatedSize = estimatedSize;
        queued.queueTime = std::chrono::steady_clock::now();
        queued.completionCallback = onComplete;
        
        // Capture packet data for sending
        queued.sendCallback = [this, connectionId, packet]() {
            // TODO: Serialize and send packet
            // This would call the actual connection send method
        };
        
        state.packetQueue.push(queued);
        return true;
    }

} // namespace Server