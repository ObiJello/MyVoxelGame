// File: src/server/network/listeners/LoginPacketListener.cpp
#include "LoginPacketListener.hpp"
#include "../ServerConnection.hpp"
#include "../NetworkServer.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketRegistry.hpp"

namespace Server {

    LoginPacketListener::LoginPacketListener(ServerConnection& connection, NetworkServer* server)
        : m_connection(connection)
        , m_server(server) {
    }

    void LoginPacketListener::onLoginStart(const Network::LoginStartC2SPacket& packet) {
        // DIAGNOSTIC: Log method entry
        Log::Info("[LoginPacketListener] *** onLoginStart() CALLED *** for player: %s", packet.username.c_str());
        Log::Debug("[LoginPacketListener] Connection ID: %u", m_connection.GetConnectionId());
        
        // Enable compression if configured (Minecraft uses 256 bytes by default)
        if (m_compressionThreshold > 0) {
            // Send SetCompression packet
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(m_compressionThreshold);
            m_connection.SendPacket(static_cast<uint8_t>(Network::PacketId::SetCompression), buffer.GetData());
            
            // Enable compression on the connection (posted to I/O thread)
            // TODO: m_connection.postEnableCompression(m_compressionThreshold);
            
            Log::Info("[LoginPacketListener] Enabled compression with threshold: %d", m_compressionThreshold);
        }
        
        // For offline/integrated mode, skip encryption and finalize login directly
        finalizeLogin(packet.username);
    }

    void LoginPacketListener::finalizeLogin(const std::string& username) {
        Log::Debug("[LoginPacketListener] finalizeLogin() called for %s", username.c_str());
        // Generate player ID (use connection ID for now)
        uint32_t playerId = m_connection.GetConnectionId();
        
        // Send LoginSuccess packet
        Network::PacketBuffer buffer;
        buffer.WriteString(std::to_string(playerId));  // UUID as string
        buffer.WriteString(username);
        
        Log::Info("[LoginPacketListener] Sending LoginSuccess for player %s (ID: %u)", 
                  username.c_str(), playerId);
        
        m_connection.SendPacket(static_cast<uint8_t>(Network::PacketId::LoginSuccess), buffer.GetData());
        
        // Mark as authenticated BEFORE switching protocol state
        m_connection.setAuthenticated(true, playerId, username);
        
        // Send initial game data
        m_connection.sendInitialGameData();
        
        // Notify server that player has joined
        if (m_server) {
            // Get shared_ptr from ServerConnection (which inherits enable_shared_from_this)
            auto connPtr = std::static_pointer_cast<ServerConnection>(m_connection.shared_from_this());
            m_server->OnPlayerJoined(connPtr);
        }
        
        // Switch to PLAY protocol state LAST
        // This replaces the current listener, so it must be done after all operations
        // that might use this LoginPacketListener instance
        m_connection.setProtocolState(Network::ProtocolState::PLAY);
        
        Log::Info("[LoginPacketListener] Player %s successfully logged in and switched to PLAY state", 
                  username.c_str());
    }

    void LoginPacketListener::onDisconnect(const std::string& reason) {
        Log::Info("[LoginPacketListener] Connection closed during login: %s", reason.c_str());
    }

} // namespace Server