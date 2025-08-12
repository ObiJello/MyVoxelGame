// File: src/server/network/NetworkServer.hpp
#pragma once

#include "common/network/NetworkConnection.hpp"
#include "common/network/AsioInclude.hpp"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

namespace Server {

    class ServerConnection;
    using ServerConnectionPtr = std::shared_ptr<ServerConnection>;
    
    // TCP server that accepts connections and manages ServerConnection instances
    class NetworkServer {
    public:
        using tcp = net::ip::tcp;
        
        // Constructor
        explicit NetworkServer(net::io_context& ioContext, uint16_t port = 25565);
        ~NetworkServer();
        
        // Non-copyable, non-movable
        NetworkServer(const NetworkServer&) = delete;
        NetworkServer& operator=(const NetworkServer&) = delete;

        // ========================================================================
        // SERVER LIFECYCLE
        // ========================================================================

        // Start accepting connections
        bool Start(const std::string& bindAddress = "127.0.0.1");
        
        // Stop accepting new connections and close all existing ones
        void Stop();
        
        // Check if server is running
        bool IsRunning() const { return m_running.load(); }
        
        // Get server port
        uint16_t GetPort() const { return m_port; }
        
        // Get bind address
        const std::string& GetBindAddress() const { return m_bindAddress; }

        // ========================================================================
        // CONNECTION MANAGEMENT
        // ========================================================================

        // Get all active connections
        std::vector<ServerConnectionPtr> GetConnections() const;
        
        // Get connection count
        size_t GetConnectionCount() const;
        
        // Disconnect a specific connection
        void DisconnectConnection(uint32_t connectionId);
        
        // Broadcast packet to all connected clients
        void BroadcastPacket(uint8_t packetId, const std::vector<uint8_t>& data);
        
        // Send packet to specific connection
        void SendPacketTo(uint32_t connectionId, uint8_t packetId, const std::vector<uint8_t>& data);
        
        // Set max connections allowed
        void SetMaxConnections(size_t maxConnections) { m_maxConnections = maxConnections; }
        size_t GetMaxConnections() const { return m_maxConnections; }

        // ========================================================================
        // CALLBACKS
        // ========================================================================

        // Connection event callbacks
        using OnConnectionCallback = std::function<void(ServerConnectionPtr)>;
        using OnDisconnectionCallback = std::function<void(ServerConnectionPtr)>;
        using OnPacketCallback = std::function<void(ServerConnectionPtr, uint8_t, const std::vector<uint8_t>&)>;
        
        // Set callbacks
        void SetOnConnection(OnConnectionCallback callback) { m_onConnection = callback; }
        void SetOnDisconnection(OnDisconnectionCallback callback) { m_onDisconnection = callback; }
        void SetOnPacket(OnPacketCallback callback) { m_onPacket = callback; }

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ServerStats {
            std::atomic<uint64_t> totalConnections{0};
            std::atomic<uint64_t> totalDisconnections{0};
            std::atomic<uint64_t> totalPacketsReceived{0};
            std::atomic<uint64_t> totalPacketsSent{0};
            std::atomic<uint64_t> totalBytesReceived{0};
            std::atomic<uint64_t> totalBytesSent{0};
            std::chrono::steady_clock::time_point startTime;
            
            void Reset() {
                totalConnections = totalDisconnections = 0;
                totalPacketsReceived = totalPacketsSent = 0;
                totalBytesReceived = totalBytesSent = 0;
                startTime = std::chrono::steady_clock::now();
            }
        };
        
        const ServerStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }

    private:
        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Start async accept
        void StartAccept();
        
        // Handle accept completion
        void HandleAccept(const error_code& error, tcp::socket socket);
        
        // Add connection to list
        void AddConnection(ServerConnectionPtr connection);
        
        // Remove connection from list
        void RemoveConnection(uint32_t connectionId);
        
        // Handle connection events (called by ServerConnection)
        friend class ServerConnection;
        void OnConnectionEstablished(ServerConnectionPtr connection);
        void OnConnectionClosed(ServerConnectionPtr connection);
        void OnPacketReceived(ServerConnectionPtr connection, uint8_t packetId, const std::vector<uint8_t>& data);
        
    public:
        // Called when a player successfully logs in
        void OnPlayerJoined(ServerConnectionPtr connection);

    private:
        // IO context and acceptor
        net::io_context& m_ioContext;
        tcp::acceptor m_acceptor;
        
        // Server configuration
        uint16_t m_port;
        std::string m_bindAddress;
        size_t m_maxConnections = 100;
        
        // Connection management
        mutable std::mutex m_connectionsMutex;
        std::vector<ServerConnectionPtr> m_connections;
        
        // Server state
        std::atomic<bool> m_running{false};
        
        // Callbacks
        OnConnectionCallback m_onConnection;
        OnDisconnectionCallback m_onDisconnection;
        OnPacketCallback m_onPacket;
        
        // Statistics
        ServerStats m_stats;
    };

} // namespace Server