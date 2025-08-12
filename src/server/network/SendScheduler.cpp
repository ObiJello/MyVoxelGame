// File: src/server/network/SendScheduler.cpp
#include "SendScheduler.hpp"
#include "ServerConnection.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <zlib.h>

namespace Server {

    SendScheduler::SendScheduler() {
        m_lastTickTime = std::chrono::steady_clock::now();
    }

    SendScheduler::~SendScheduler() {
        Shutdown();
    }

    // === INITIALIZATION ===

    void SendScheduler::Initialize(const Config& config) {
        m_config = config;
        m_initialized = true;
        
        Log::Info("SendScheduler: Initialized with %.1f MB global limit per tick",
                 m_config.globalMaxBytesPerTick / 1048576.0);
    }

    void SendScheduler::Shutdown() {
        if (!m_initialized) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        // Clear all queues
        for (auto& [connId, state] : m_connectionStates) {
            while (!state.packetQueue.empty()) {
                state.packetQueue.pop();
            }
        }
        
        m_connectionStates.clear();
        m_connections.clear();
        m_initialized = false;
    }

    // === CONNECTION MANAGEMENT ===

    void SendScheduler::RegisterConnection(uint32_t connectionId, std::shared_ptr<ServerConnection> connection) {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        ConnectionSendState state;
        state.connectionId = connectionId;
        state.maxOutboxBytes = m_config.defaultMaxOutboxBytes;
        state.lastSendTime = std::chrono::steady_clock::now();
        
        m_connectionStates[connectionId] = state;
        m_connections[connectionId] = connection;
        
        Log::Info("SendScheduler: Registered connection %u", connectionId);
    }

    void SendScheduler::UnregisterConnection(uint32_t connectionId) {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        m_connectionStates.erase(connectionId);
        m_connections.erase(connectionId);
        
        Log::Info("SendScheduler: Unregistered connection %u", connectionId);
    }

    void SendScheduler::SetConnectionLimits(uint32_t connectionId, size_t maxOutboxBytes) {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        auto it = m_connectionStates.find(connectionId);
        if (it != m_connectionStates.end()) {
            it->second.maxOutboxBytes = maxOutboxBytes;
        }
    }

    ConnectionSendState* SendScheduler::GetConnectionState(uint32_t connectionId) {
        auto it = m_connectionStates.find(connectionId);
        if (it != m_connectionStates.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // === PACKET ENQUEUEING ===

    bool SendScheduler::EnqueueChunkData(
        uint32_t connectionId,
        const Network::ChunkDataS2CPacket& packet,
        std::function<void()> onComplete
    ) {
        size_t estimatedSize = packet.CalculateDataSize() + 32;
        return EnqueuePacket(connectionId, packet, PacketPriority::NORMAL, estimatedSize, onComplete);
    }

    bool SendScheduler::EnqueueChunkUnload(
        uint32_t connectionId,
        const Network::UnloadChunkS2CPacket& packet
    ) {
        return EnqueuePacket(connectionId, packet, PacketPriority::HIGH, 16, nullptr);
    }

    bool SendScheduler::EnqueueBlockChange(
        uint32_t connectionId,
        const Network::BlockChangeS2CPacket& packet
    ) {
        return EnqueuePacket(connectionId, packet, PacketPriority::HIGH, 32, nullptr);
    }

    bool SendScheduler::EnqueueMultiBlockChange(
        uint32_t connectionId,
        const Network::MultiBlockChangeS2CPacket& packet
    ) {
        size_t estimatedSize = 16 + packet.changes.size() * 8;
        return EnqueuePacket(connectionId, packet, PacketPriority::HIGH, estimatedSize, nullptr);
    }

    // === SENDING ===

    void SendScheduler::ProcessSendQueues() {
        auto now = std::chrono::steady_clock::now();
        
        // Reset global counters
        m_globalBytesSentThisTick = 0;
        m_globalPacketsSentThisTick = 0;
        
        // Calculate global budgets
        size_t globalBytesRemaining = m_config.globalMaxBytesPerTick;
        size_t globalPacketsRemaining = m_config.maxGlobalPacketsPerTick;
        
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        // Process each connection
        for (auto& [connId, state] : m_connectionStates) {
            // Reset per-tick counters
            state.bytesSentThisTick = 0;
            state.packetsSentThisTick = 0;
            
            // Skip if no global budget left
            if (globalBytesRemaining == 0 || globalPacketsRemaining == 0) {
                break;
            }
            
            // Process this connection's queue
            ProcessConnectionQueue(connId, state, globalBytesRemaining, globalPacketsRemaining);
            
            // Update statistics
            m_stats.totalBytesSent += state.bytesSentThisTick;
            m_stats.totalPacketsSent += state.packetsSentThisTick;
        }
        
        m_lastTickTime = now;
    }

    void SendScheduler::FlushConnection(uint32_t connectionId) {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        auto it = m_connectionStates.find(connectionId);
        if (it == m_connectionStates.end()) {
            return;
        }
        
        auto& state = it->second;
        size_t maxPackets = 100;  // Limit to prevent blocking
        
        while (!state.packetQueue.empty() && maxPackets > 0) {
            auto packet = state.packetQueue.top();
            state.packetQueue.pop();
            
            SendPacketToConnection(connectionId, packet);
            maxPackets--;
        }
    }

    void SendScheduler::SendCriticalPackets() {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        for (auto& [connId, state] : m_connectionStates) {
            // Create temporary queue for non-critical packets
            std::priority_queue<QueuedPacket> tempQueue;
            
            // Process queue, sending critical packets
            while (!state.packetQueue.empty()) {
                auto packet = state.packetQueue.top();
                state.packetQueue.pop();
                
                if (packet.priority == PacketPriority::CRITICAL) {
                    SendPacketToConnection(connId, packet);
                } else {
                    tempQueue.push(packet);
                }
            }
            
            // Restore non-critical packets
            state.packetQueue = std::move(tempQueue);
        }
    }

    // === BACK-PRESSURE ===

    bool SendScheduler::IsBackpressured(uint32_t connectionId) const {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        auto it = m_connectionStates.find(connectionId);
        if (it != m_connectionStates.end()) {
            return it->second.isBackpressured;
        }
        
        return false;
    }

    void SendScheduler::UpdateOutboxSize(uint32_t connectionId, size_t newSize) {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        auto it = m_connectionStates.find(connectionId);
        if (it != m_connectionStates.end()) {
            auto& state = it->second;
            state.outboxBytes = newSize;
            
            // Update back-pressure state
            bool wasBackpressured = state.isBackpressured;
            state.isBackpressured = (newSize >= state.maxOutboxBytes * 0.8);
            
            if (wasBackpressured && !state.isBackpressured) {
                Log::Debug("SendScheduler: Connection %u back-pressure relieved", connectionId);
            } else if (!wasBackpressured && state.isBackpressured) {
                Log::Warning("SendScheduler: Connection %u is back-pressured", connectionId);
            }
            
            // Update peak size stat
            if (newSize > m_stats.peakOutboxSize) {
                m_stats.peakOutboxSize = newSize;
            }
        }
    }

    void SendScheduler::OnWriteComplete(uint32_t connectionId, size_t bytesWritten) {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        auto it = m_connectionStates.find(connectionId);
        if (it != m_connectionStates.end()) {
            auto& state = it->second;
            
            // Update outbox size
            if (state.outboxBytes >= bytesWritten) {
                state.outboxBytes -= bytesWritten;
            } else {
                state.outboxBytes = 0;
            }
            
            // Update bandwidth calculation
            UpdateBandwidth(state, bytesWritten);
            
            // Update stats
            state.totalBytesSent += bytesWritten;
        }
    }

    // === STATISTICS ===

    SendScheduler::Stats SendScheduler::GetStats() const {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        Stats stats = m_stats;
        stats.totalConnections = m_connectionStates.size();
        
        // Count back-pressured connections and calculate average bandwidth
        float totalBandwidth = 0;
        stats.backpressuredConnections = 0;
        stats.totalQueuedPackets = 0;
        
        for (const auto& [connId, state] : m_connectionStates) {
            if (state.isBackpressured) {
                stats.backpressuredConnections++;
            }
            stats.totalQueuedPackets += state.packetQueue.size();
            totalBandwidth += state.bandwidth;
        }
        
        if (!m_connectionStates.empty()) {
            stats.averageBandwidth = totalBandwidth / m_connectionStates.size();
        }
        
        return stats;
    }

    void SendScheduler::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }

    ConnectionSendState SendScheduler::GetConnectionStats(uint32_t connectionId) const {
        std::lock_guard<std::mutex> lock(m_connectionMutex);
        
        auto it = m_connectionStates.find(connectionId);
        if (it != m_connectionStates.end()) {
            return it->second;
        }
        
        return ConnectionSendState{};
    }

    // === CONFIGURATION ===

    void SendScheduler::SetConfig(const Config& config) {
        m_config = config;
    }

    const SendScheduler::Config& SendScheduler::GetConfig() const {
        return m_config;
    }

    // === INTERNAL METHODS ===

    void SendScheduler::ProcessConnectionQueue(
        uint32_t connectionId,
        ConnectionSendState& state,
        size_t& globalBytesRemaining,
        size_t& globalPacketsRemaining
    ) {
        // Drop old packets if configured
        if (m_config.dropOldPackets) {
            DropOldPackets(state);
        }
        
        // Calculate per-connection limits
        size_t connBytesRemaining = state.maxOutboxBytes - state.outboxBytes;
        size_t connPacketsRemaining = m_config.maxPacketsPerConnectionPerTick;
        
        // Process packets
        while (!state.packetQueue.empty() &&
               connBytesRemaining > 0 &&
               connPacketsRemaining > 0 &&
               globalBytesRemaining > 0 &&
               globalPacketsRemaining > 0) {
            
            auto packet = state.packetQueue.top();
            
            // Check if packet fits in budget
            if (packet.estimatedSize > connBytesRemaining ||
                packet.estimatedSize > globalBytesRemaining) {
                // Skip large packets if they don't fit
                if (packet.priority >= PacketPriority::LOW) {
                    break;
                }
            }
            
            state.packetQueue.pop();
            
            // Send packet
            if (SendPacketToConnection(connectionId, packet)) {
                // Update budgets
                size_t packetSize = packet.estimatedSize;
                connBytesRemaining -= std::min(connBytesRemaining, packetSize);
                globalBytesRemaining -= std::min(globalBytesRemaining, packetSize);
                connPacketsRemaining--;
                globalPacketsRemaining--;
                
                // Update counters
                state.bytesSentThisTick += packetSize;
                state.packetsSentThisTick++;
                state.outboxBytes += packetSize;
                
                // Call completion callback if provided
                if (packet.completionCallback) {
                    packet.completionCallback();
                }
            }
        }
    }

    bool SendScheduler::SendPacketToConnection(
        uint32_t connectionId,
        const QueuedPacket& packet
    ) {
        auto connIt = m_connections.find(connectionId);
        if (connIt == m_connections.end()) {
            return false;
        }
        
        // Execute send callback
        if (packet.sendCallback) {
            packet.sendCallback();
            return true;
        }
        
        return false;
    }

    void SendScheduler::DropOldPackets(ConnectionSendState& state) {
        auto now = std::chrono::steady_clock::now();
        std::vector<QueuedPacket> keepPackets;
        
        // Check each packet's age
        while (!state.packetQueue.empty()) {
            auto packet = state.packetQueue.top();
            state.packetQueue.pop();
            
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - packet.queueTime);
            
            // Keep critical and high priority packets, drop old low priority ones
            if (packet.priority <= PacketPriority::HIGH || age < m_config.packetTimeout) {
                keepPackets.push_back(packet);
            } else {
                state.packetsDropped++;
                m_stats.totalPacketsDropped++;
            }
        }
        
        // Re-add kept packets
        for (const auto& packet : keepPackets) {
            state.packetQueue.push(packet);
        }
    }

    std::vector<uint8_t> SendScheduler::CompressPacket(const std::vector<uint8_t>& data) {
        if (!m_config.enableCompression || data.size() < static_cast<size_t>(m_config.compressionThreshold)) {
            return data;
        }
        
        // Compress using zlib
        uLong compressedSize = compressBound(data.size());
        std::vector<uint8_t> compressed(compressedSize);
        
        int result = compress2(
            compressed.data(),
            &compressedSize,
            data.data(),
            data.size(),
            Z_DEFAULT_COMPRESSION
        );
        
        if (result == Z_OK) {
            compressed.resize(compressedSize);
            return compressed;
        }
        
        // Return original if compression failed
        return data;
    }

    void SendScheduler::UpdateBandwidth(ConnectionSendState& state, size_t bytesSent) {
        auto now = std::chrono::steady_clock::now();
        auto timeDelta = std::chrono::duration<float>(now - state.lastSendTime).count();
        
        if (timeDelta > 0) {
            float instantBandwidth = bytesSent / timeDelta;
            // Exponential moving average
            state.bandwidth = state.bandwidth * 0.9f + instantBandwidth * 0.1f;
        }
        
        state.lastSendTime = now;
    }

} // namespace Server