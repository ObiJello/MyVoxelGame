// File: src/client/network/NetworkClient.hpp
#pragma once

#include "common/network/NetworkConnection.hpp"
#include "common/network/AsioInclude.hpp"
#include <memory>
#include <string>
#include <atomic>
#include <functional>

namespace Client {

    class ClientConnection;
    using ClientConnectionPtr = std::shared_ptr<ClientConnection>;
    
    // TCP client that connects to a server
    class ClientPacketHandler;
    
    class NetworkClient {
    public:
        using tcp = net::ip::tcp;
        
        // Constructor
        explicit NetworkClient(net::io_context& ioContext);
        ~NetworkClient();
        
        // Non-copyable, non-movable
        NetworkClient(const NetworkClient&) = delete;
        NetworkClient& operator=(const NetworkClient&) = delete;

        // ========================================================================
        // CLIENT LIFECYCLE
        // ========================================================================

        // Connect to server
        bool Connect(const std::string& host, uint16_t port = 25565);
        
        // Async connect to server
        void ConnectAsync(const std::string& host, uint16_t port = 25565);
        
        // Disconnect from server
        void Disconnect();
        
        // Check if connected
        bool IsConnected() const;
        
        // Get connection
        ClientConnectionPtr GetConnection() const { return m_connection; }
        
        // Get packet handler for main thread packet processing
        std::shared_ptr<ClientPacketHandler> GetPacketHandler() const { return m_packetHandler; }
        
        // Drain incoming packets on main thread
        void DrainIncomingPackets();

        // ========================================================================
        // CALLBACKS
        // ========================================================================

        // Connection event callbacks
        using OnConnectedCallback = std::function<void()>;
        using OnDisconnectedCallback = std::function<void(const std::string& reason)>;
        using OnPacketCallback = std::function<void(uint8_t, const std::vector<uint8_t>&)>;
        using OnErrorCallback = std::function<void(const std::string& error)>;
        
        // Set callbacks
        void SetOnConnected(OnConnectedCallback callback) { m_onConnected = callback; }
        void SetOnDisconnected(OnDisconnectedCallback callback) { m_onDisconnected = callback; }
        void SetOnPacket(OnPacketCallback callback) { m_onPacket = callback; }
        void SetOnError(OnErrorCallback callback) { m_onError = callback; }
        
        // Clear all callbacks (used during shutdown to prevent crashes)
        void ClearCallbacks();

        // ========================================================================
        // CONNECTION INFO
        // ========================================================================
        
        // Get server address
        const std::string& GetServerHost() const { return m_serverHost; }
        uint16_t GetServerPort() const { return m_serverPort; }

        // Set player name for handshake (default: "Player1")
        void SetPlayerName(const std::string& name) { m_playerName = name; }
        const std::string& GetPlayerName() const { return m_playerName; }
        
        // Server-sent view distance (effective render distance capped by server)
        void SetServerViewDistance(int distance) { m_serverViewDistance.store(distance); }
        int GetServerViewDistance() const { return m_serverViewDistance.load(); }

        // Get connection state
        enum class ClientState {
            IDLE,
            CONNECTING,
            CONNECTED,
            FAILED,
            DISCONNECTED
        };
        ClientState GetState() const { return m_state.load(); }

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ClientStats {
            std::atomic<uint64_t> packetsReceived{0};
            std::atomic<uint64_t> packetsSent{0};
            std::atomic<uint64_t> bytesReceived{0};
            std::atomic<uint64_t> bytesSent{0};
            std::chrono::steady_clock::time_point connectedTime;
            std::chrono::steady_clock::time_point disconnectedTime;
            
            void Reset() {
                packetsReceived = packetsSent = 0;
                bytesReceived = bytesSent = 0;
            }
        };
        
        const ClientStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }

    private:
        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Handle connection result
        void HandleConnect(const error_code& error);
        
        // Connection event handlers (called by ClientConnection)
        friend class ClientConnection;
        void OnConnectionEstablished();
        void OnConnectionClosed(const std::string& reason);
        void OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& data);
        void OnConnectionError(const std::string& error);
        
        // One-shot completion helpers
        void CompleteSuccess();
        void CompleteError(const std::string& error);

    private:
        // IO context
        net::io_context& m_ioContext;
        
        // Connection
        ClientConnectionPtr m_connection;
        tcp::resolver m_resolver;
        
        // Server info
        std::string m_serverHost;
        uint16_t m_serverPort = 25565;

        // Player name for handshake (empty → server auto-assigns "PlayerN")
        std::string m_playerName;
        
        // Client state
        std::atomic<ClientState> m_state{ClientState::IDLE};
        std::atomic_flag m_completionFlag = ATOMIC_FLAG_INIT;
        
        // Callbacks
        OnConnectedCallback m_onConnected;
        OnDisconnectedCallback m_onDisconnected;
        OnPacketCallback m_onPacket;
        OnErrorCallback m_onError;
        
        // Server-sent view distance (-1 = not yet received)
        std::atomic<int> m_serverViewDistance{-1};

        // Statistics
        ClientStats m_stats;

        // Packet handler for main thread processing
        std::shared_ptr<ClientPacketHandler> m_packetHandler;
    };
    
    // Global instance (temporary - prefer dependency injection)
    extern NetworkClient* g_networkClient;

} // namespace Client