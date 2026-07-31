// File: src/server/network/NetworkServer.cpp
#include "NetworkServer.hpp"
#include "ServerConnection.hpp"
#include "../IntegratedServer.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>   // ::close for rejected relay handles
#endif

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

            // Read back actual port (important when binding to port 0)
            m_port = m_acceptor.local_endpoint().port();

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
        
        SetupConnection(std::move(socket), "accepted");

        // Continue accepting
        StartAccept();
    }

    void NetworkServer::SetupConnection(tcp::socket socket, const char* origin) {
        // Set TCP_NODELAY to disable Nagle's algorithm for low-latency
        // This is critical for real-time game networking, especially on Windows
        try {
            socket.set_option(tcp::no_delay(true));
        } catch (const std::exception& e) {
            Log::Warning("Failed to set TCP_NODELAY: %s", e.what());
        }

        // Create new ServerConnection
        auto connection = std::make_shared<ServerConnection>(std::move(socket), this);

        // Add to connections list
        AddConnection(connection);

        // Start the connection
        connection->Start();

        std::string remote = "?";
        try {
            remote = connection->GetRemoteEndpoint().address().to_string();
        } catch (const std::exception&) {
            // Relay tunnels report the relay's address; not fatal either way.
        }
        Log::Info("New connection %s from %s (ID: %u)", origin, remote.c_str(),
                  connection->GetConnectionId());
    }

    void NetworkServer::AdoptConnection(tcp::socket::native_handle_type handle) {
        // Runs on the caller's thread (the friends-client io thread), so hop
        // to the server's io context before touching server state.
        net::post(m_ioContext, [this, handle]() {
            if (GetConnectionCount() >= m_maxConnections) {
                Log::Warning("Max connections reached, rejecting relay tunnel");
#ifdef _WIN32
                (void)::closesocket(handle);
#else
                (void)::close(handle);
#endif
                return;
            }
            tcp::socket socket(m_ioContext);
            error_code ec;
            socket.assign(tcp::v4(), handle, ec);
            if (ec) {
                Log::Error("Failed to adopt relay socket: %s", ec.message().c_str());
                return;
            }
            SetupConnection(std::move(socket), "adopted (relay)");
        });
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