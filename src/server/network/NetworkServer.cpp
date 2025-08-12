// File: src/server/network/NetworkServer.cpp
#include "NetworkServer.hpp"
#include "ServerConnection.hpp"
#include "../IntegratedServer.hpp"
#include "common/core/Log.hpp"
#include <algorithm>

namespace Server {

    NetworkServer::NetworkServer(net::io_context& ioContext, uint16_t port)
        : m_ioContext(ioContext)
        , m_acceptor(ioContext)
        , m_port(port)
    {
        m_stats.startTime = std::chrono::steady_clock::now();
    }

    NetworkServer::~NetworkServer() {
        Stop();
    }

    bool NetworkServer::Start(const std::string& bindAddress) {
        if (m_running.load()) {
            Log::Warning("NetworkServer already running");
            return false;
        }
        
        m_bindAddress = bindAddress;
        
        try {
            // Create endpoint
            tcp::endpoint endpoint(net::ip::make_address(bindAddress), m_port);
            
            // Open acceptor
            m_acceptor.open(endpoint.protocol());
            m_acceptor.set_option(tcp::acceptor::reuse_address(true));
            m_acceptor.bind(endpoint);
            m_acceptor.listen();
            
            m_running.store(true);
            
            Log::Info("NetworkServer listening on %s:%d", bindAddress.c_str(), m_port);
            
            // Start accepting connections
            StartAccept();
            
            return true;
        } catch (const std::exception& e) {
            Log::Error("Failed to start NetworkServer: %s", e.what());
            return false;
        }
    }

    void NetworkServer::Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        
        Log::Info("Stopping NetworkServer...");
        
        // Stop accepting new connections
        error_code ec;
        m_acceptor.close(ec);
        
        // Disconnect all clients
        std::vector<ServerConnectionPtr> connections;
        {
            std::lock_guard<std::mutex> lock(m_connectionsMutex);
            connections = m_connections;
        }
        
        for (auto& conn : connections) {
            conn->Disconnect();
        }
        
        // Clear connections list
        {
            std::lock_guard<std::mutex> lock(m_connectionsMutex);
            m_connections.clear();
        }
        
        Log::Info("NetworkServer stopped");
    }

    void NetworkServer::StartAccept() {
        if (!m_running.load()) {
            return;
        }
        
        // Create a new socket for the incoming connection
        auto socket = std::make_shared<tcp::socket>(m_ioContext);
        
        m_acceptor.async_accept(*socket,
            [this, socket](const error_code& error) {
                if (!error) {
                    HandleAccept(error, std::move(*socket));
                } else if (m_running.load()) {
                    Log::Error("Accept error: %s", error.message().c_str());
                    // Continue accepting despite error
                    StartAccept();
                }
            });
    }

    void NetworkServer::HandleAccept(const error_code& error, tcp::socket socket) {
        if (error) {
            Log::Error("HandleAccept error: %s", error.message().c_str());
            StartAccept();
            return;
        }
        
        // Check max connections
        if (GetConnectionCount() >= m_maxConnections) {
            Log::Warning("Max connections reached, rejecting new connection");
            socket.close();
            StartAccept();
            return;
        }
        
        // Create new ServerConnection
        auto connection = std::make_shared<ServerConnection>(std::move(socket), this);
        
        // Add to connections list
        AddConnection(connection);
        
        // Start the connection
        connection->Start();
        
        Log::Info("New connection accepted from %s (ID: %u)",
            connection->GetRemoteEndpoint().address().to_string().c_str(),
            connection->GetConnectionId());
        
        // Continue accepting
        StartAccept();
    }

    void NetworkServer::AddConnection(ServerConnectionPtr connection) {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        m_connections.push_back(connection);
        m_stats.totalConnections.fetch_add(1);
    }

    void NetworkServer::RemoveConnection(uint32_t connectionId) {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        m_connections.erase(
            std::remove_if(m_connections.begin(), m_connections.end(),
                [connectionId](const ServerConnectionPtr& conn) {
                    return conn->GetConnectionId() == connectionId;
                }),
            m_connections.end()
        );
        m_stats.totalDisconnections.fetch_add(1);
    }

    std::vector<ServerConnectionPtr> NetworkServer::GetConnections() const {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        return m_connections;
    }

    size_t NetworkServer::GetConnectionCount() const {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        return m_connections.size();
    }

    void NetworkServer::DisconnectConnection(uint32_t connectionId) {
        ServerConnectionPtr connection;
        {
            std::lock_guard<std::mutex> lock(m_connectionsMutex);
            auto it = std::find_if(m_connections.begin(), m_connections.end(),
                [connectionId](const ServerConnectionPtr& conn) {
                    return conn->GetConnectionId() == connectionId;
                });
            if (it != m_connections.end()) {
                connection = *it;
            }
        }
        
        if (connection) {
            connection->Disconnect();
        }
    }

    void NetworkServer::BroadcastPacket(uint8_t packetId, const std::vector<uint8_t>& data) {
        /*Log::Info("[NetworkServer] BroadcastPacket called with packetId=0x%02X (%u), data size=%zu",
                  packetId, packetId, data.size());*/
        
        auto connections = GetConnections();
        //Log::Debug("[NetworkServer] Broadcasting to %zu connections", connections.size());
        
        for (auto& conn : connections) {
            if (conn->IsConnected() && conn->IsAuthenticated()) {
                Log::Debug("[NetworkServer] Sending to connection %u, packetId=0x%02X", 
                          conn->GetConnectionId(), packetId);
                conn->SendPacket(packetId, data);
            }
        }
        m_stats.totalPacketsSent.fetch_add(connections.size());
    }

    void NetworkServer::SendPacketTo(uint32_t connectionId, uint8_t packetId, const std::vector<uint8_t>& data) {
        ServerConnectionPtr connection;
        {
            std::lock_guard<std::mutex> lock(m_connectionsMutex);
            auto it = std::find_if(m_connections.begin(), m_connections.end(),
                [connectionId](const ServerConnectionPtr& conn) {
                    return conn->GetConnectionId() == connectionId;
                });
            if (it != m_connections.end()) {
                connection = *it;
            }
        }
        
        if (connection && connection->IsConnected()) {
            connection->SendPacket(packetId, data);
            m_stats.totalPacketsSent.fetch_add(1);
        }
    }

    void NetworkServer::OnConnectionEstablished(ServerConnectionPtr connection) {
        if (m_onConnection) {
            m_onConnection(connection);
        }
    }

    void NetworkServer::OnConnectionClosed(ServerConnectionPtr connection) {
        RemoveConnection(connection->GetConnectionId());
        if (m_onDisconnection) {
            m_onDisconnection(connection);
        }
    }

    void NetworkServer::OnPacketReceived(ServerConnectionPtr connection, uint8_t packetId, const std::vector<uint8_t>& data) {
        m_stats.totalPacketsReceived.fetch_add(1);
        m_stats.totalBytesReceived.fetch_add(data.size());
        
        if (m_onPacket) {
            m_onPacket(connection, packetId, data);
        }
    }
    
    void NetworkServer::OnPlayerJoined(ServerConnectionPtr connection) {
        Log::Info("[NetworkServer] Player joined: connection %u", connection->GetConnectionId());
        
        // Notify IntegratedServer to send initial chunks
        if (Server::g_integratedServer) {
            Server::g_integratedServer->OnPlayerJoined(connection);
        }
    }

} // namespace Server