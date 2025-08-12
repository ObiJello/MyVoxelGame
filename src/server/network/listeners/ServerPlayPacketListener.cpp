// File: src/server/network/listeners/ServerPlayPacketListener.cpp
#include "ServerPlayPacketListener.hpp"
#include "../ServerConnection.hpp"
#include "common/core/Log.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"

namespace Server {
    
    ServerPlayPacketListener::ServerPlayPacketListener(ServerConnection& connection)
        : m_connection(connection) {
        Log::Debug("[ServerPlayPacketListener] Created for connection %u", 
                   m_connection.GetConnectionId());
    }
    
    void ServerPlayPacketListener::onKeepAliveResponse(const Network::KeepAliveC2SPacket& packet) {
        // Create a temporary payload for the legacy handler
        // This is a temporary solution until we fully migrate to typed packets
        Network::PacketBuffer buffer;
        buffer.WriteLong(packet.keepAliveId);
        
        // Call the existing handler
        m_connection.HandleKeepAliveResponse(buffer.GetData());
    }
    
} // namespace Server