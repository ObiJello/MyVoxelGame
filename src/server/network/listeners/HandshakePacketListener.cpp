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
        
        // Refuse a mismatched peer outright. This used to log a warning and
        // fall through, which was harmless only while the two ends could not
        // meaningfully disagree. They can now: the chunk stream is positional,
        // so a version skew does not fail to parse, it parses into the wrong
        // sections. See Network::kProtocolVersion.
        if (packet.protocolVersion != Network::kProtocolVersion) {
            Log::Warning("[HandshakePacketListener] Rejecting protocol version %d (need %d)",
                         packet.protocolVersion, Network::kProtocolVersion);
            m_connection.SendDisconnect(
                packet.protocolVersion < Network::kProtocolVersion
                    ? "Your game is out of date — update it to join this server."
                    : "This server is out of date — it cannot accept your client version.");
            return;
        }
        
        // Switch to the requested state
        switch (packet.nextState) {
            case static_cast<int32_t>(Network::NextStateWire::STATUS):
                Log::Info("[HandshakePacketListener] Switching to STATUS state");
                m_connection.setProtocolState(Network::ProtocolState::STATUS);
                break;
                
            case static_cast<int32_t>(Network::NextStateWire::LOGIN):
                Log::Info("[HandshakePacketListener] Switching to LOGIN state");
                m_connection.setProtocolState(Network::ProtocolState::LOGIN);
                break;
                
            default:
                Log::Error("[HandshakePacketListener] Invalid next state: %d", packet.nextState);
                m_connection.SendDisconnect("Invalid handshake state");
                break;
        }
    }

    void HandshakePacketListener::onDisconnect(const std::string& reason) {
        Log::Info("[HandshakePacketListener] Connection closed: %s", reason.c_str());
    }

} // namespace Server