// File: src/server/network/listeners/HandshakePacketListener.cpp
#include "HandshakePacketListener.hpp"
#include "../ServerConnection.hpp"
#include "common/core/Log.hpp"

namespace Server {

    HandshakePacketListener::HandshakePacketListener(ServerConnection& connection)
        : m_connection(connection) {
    }

    void HandshakePacketListener::onHandshake(const Network::HandshakeC2SPacket& packet) {
        Log::Info("[HandshakePacketListener] Received handshake: protocol=%d, address=%s:%u, nextState=%d",
                  packet.protocolVersion, packet.serverAddress.c_str(), packet.serverPort, 
                  static_cast<int>(packet.nextState));
        
        // Validate protocol version (754 = Minecraft 1.16.5)
        const int SUPPORTED_PROTOCOL = 754;
        if (packet.protocolVersion != SUPPORTED_PROTOCOL) {
            Log::Warning("[HandshakePacketListener] Unsupported protocol version: %d", packet.protocolVersion);
            // For now, allow any version for testing
        }
        
        // Switch to the requested state
        switch (packet.nextState) {
            case Network::ProtocolState::STATUS:
                Log::Info("[HandshakePacketListener] Switching to STATUS state");
                m_connection.setProtocolState(Network::ProtocolState::STATUS);
                break;
                
            case Network::ProtocolState::LOGIN:
                Log::Info("[HandshakePacketListener] Switching to LOGIN state");
                m_connection.setProtocolState(Network::ProtocolState::LOGIN);
                break;
                
            default:
                Log::Error("[HandshakePacketListener] Invalid next state: %d", static_cast<int>(packet.nextState));
                m_connection.SendDisconnect("Invalid handshake state");
                break;
        }
    }

    void HandshakePacketListener::onDisconnect(const std::string& reason) {
        Log::Info("[HandshakePacketListener] Connection closed: %s", reason.c_str());
    }

} // namespace Server