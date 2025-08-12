// File: src/client/network/NetworkClient.cpp
#include "NetworkClient.hpp"
#include "ClientConnection.hpp"
#include "ClientPacketHandler.hpp"
#include "common/core/Log.hpp"

namespace Client {

    // Global instance
    NetworkClient* g_networkClient = nullptr;

    NetworkClient::NetworkClient(net::io_context& ioContext)
        : m_ioContext(ioContext)
        , m_resolver(ioContext)
        , m_packetHandler(std::make_shared<ClientPacketHandler>())
    {
    }

    NetworkClient::~NetworkClient() {
        Disconnect();
    }

    bool NetworkClient::Connect(const std::string& host, uint16_t port) {
        if (m_state != ClientState::IDLE && m_state != ClientState::DISCONNECTED) {
            Log::Warning("NetworkClient: Already connected or connecting");
            return false;
        }
        
        m_serverHost = host;
        m_serverPort = port;
        m_state = ClientState::CONNECTING;
        
        try {
            // Resolve server address
            tcp::resolver::results_type endpoints = m_resolver.resolve(host, std::to_string(port));
            
            // Create socket
            tcp::socket socket(m_ioContext);
            
            // Connect synchronously
            net::connect(socket, endpoints);
            
            // Create ClientConnection
            m_connection = std::make_shared<ClientConnection>(std::move(socket), this);
            m_connection->Start();
            
            m_state = ClientState::CONNECTED;
            m_stats.connectedTime = std::chrono::steady_clock::now();
            
            Log::Info("NetworkClient: Connected to %s:%u", host.c_str(), port);
            
            OnConnectionEstablished();
            return true;
            
        } catch (const std::exception& e) {
            Log::Error("NetworkClient: Failed to connect to %s:%u - %s", 
                host.c_str(), port, e.what());
            m_state = ClientState::DISCONNECTED;
            OnConnectionError(e.what());
            return false;
        }
    }

    void NetworkClient::ConnectAsync(const std::string& host, uint16_t port) {
        ClientState expected = ClientState::IDLE;
        if (!m_state.compare_exchange_strong(expected, ClientState::CONNECTING)) {
            expected = ClientState::DISCONNECTED;
            if (!m_state.compare_exchange_strong(expected, ClientState::CONNECTING)) {
                Log::Warning("NetworkClient: Already connecting or connected");
                return;
            }
        }
        
        // Reset completion flag for new connection attempt
        m_completionFlag.clear();
        
        m_serverHost = host;
        m_serverPort = port;
        
        Log::Info("NetworkClient: Connecting to %s:%u...", host.c_str(), port);
        
        // Resolve server address asynchronously
        m_resolver.async_resolve(host, std::to_string(port),
            [this](const error_code& error, tcp::resolver::results_type endpoints) {
                if (error) {
                    Log::Error("NetworkClient: Failed to resolve server: %s", error.message().c_str());
                    CompleteError(error.message());
                    return;
                }
                
                // Create socket and connect
                auto socket = std::make_shared<tcp::socket>(m_ioContext);
                net::async_connect(*socket, endpoints,
                    [this, socket](const error_code& error, const tcp::endpoint&) {
                        if (error) {
                            HandleConnect(error);
                        } else {
                            // Create ClientConnection
                            m_connection = std::make_shared<ClientConnection>(std::move(*socket), this);
                            m_connection->Start();
                            
                            // CRITICAL: Send handshake immediately to prevent server EOF
                            m_connection->StartHandshake("Player1");
                            
                            m_stats.connectedTime = std::chrono::steady_clock::now();
                            
                            Log::Info("NetworkClient: Connected to %s:%u", 
                                m_serverHost.c_str(), m_serverPort);
                            
                            CompleteSuccess();
                        }
                    });
            });
    }

    void NetworkClient::Disconnect() {
        if (m_state == ClientState::DISCONNECTED) {
            return;
        }
        
        Log::Info("NetworkClient: Disconnecting from %s:%u", 
            m_serverHost.c_str(), m_serverPort);
        
        if (m_connection) {
            m_connection->Close();
            m_connection.reset();
        }
        
        m_state = ClientState::DISCONNECTED;
        m_stats.disconnectedTime = std::chrono::steady_clock::now();
    }

    bool NetworkClient::IsConnected() const {
        return m_state == ClientState::CONNECTED && 
               m_connection && 
               m_connection->IsConnected();
    }

    void NetworkClient::HandleConnect(const error_code& error) {
        if (error) {
            Log::Error("NetworkClient: Connection failed: %s", error.message().c_str());
            CompleteError(error.message());
        }
        // Success case is handled in the async_connect lambda
    }

    void NetworkClient::OnConnectionEstablished() {
        if (m_onConnected) {
            m_onConnected();
        }
    }

    void NetworkClient::OnConnectionClosed(const std::string& reason) {
        Log::Info("NetworkClient: Connection closed: %s", reason.c_str());
        
        m_state = ClientState::DISCONNECTED;
        m_connection.reset();
        m_stats.disconnectedTime = std::chrono::steady_clock::now();
        
        if (m_onDisconnected) {
            m_onDisconnected(reason);
        }
    }

    void NetworkClient::OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& data) {
        m_stats.packetsReceived.fetch_add(1);
        m_stats.bytesReceived.fetch_add(data.size());
        
        if (m_onPacket) {
            m_onPacket(packetId, data);
        }
    }

    void NetworkClient::OnConnectionError(const std::string& error) {
        if (m_onError) {
            m_onError(error);
        }
    }
    
    void NetworkClient::DrainIncomingPackets() {
        if (m_connection && m_connection->IsConnected()) {
            m_connection->DrainIncomingPackets();
        }
    }
    
    void NetworkClient::CompleteSuccess() {
        if (!m_completionFlag.test_and_set()) {
            m_state = ClientState::CONNECTED;
            if (m_onConnected) {
                m_onConnected();
            }
        }
    }
    
    void NetworkClient::CompleteError(const std::string& error) {
        if (!m_completionFlag.test_and_set()) {
            m_state = ClientState::FAILED;
            if (m_onError) {
                m_onError(error);
            }
        }
    }

} // namespace Client