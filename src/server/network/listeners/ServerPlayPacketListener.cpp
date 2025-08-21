// File: src/server/network/listeners/ServerPlayPacketListener.cpp
#include "ServerPlayPacketListener.hpp"
#include "../ServerConnection.hpp"
#include "../../session/PlayerSession.hpp"
#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include "common/network/PacketTypes.hpp"

namespace Server {
    
    ServerPlayPacketListener::ServerPlayPacketListener(ServerConnection& connection, PlayerSession& session)
        : m_connection(connection), m_session(session) {
        Log::Debug("[ServerPlayPacketListener] Created for connection %u with session %u", 
                   m_connection.GetConnectionId(), session.GetPlayerId());
    }
    
    void ServerPlayPacketListener::onKeepAliveResponse(const Network::KeepAliveC2SPacket& packet) {
        // Create a temporary payload for the legacy handler
        // This is a temporary solution until we fully migrate to typed packets
        Network::PacketBuffer buffer;
        buffer.WriteLong(packet.keepAliveId);
        
        // Call the existing handler
        m_connection.HandleKeepAliveResponse(buffer.GetData());
    }
    
    void ServerPlayPacketListener::handleUseItemOn(const Network::UseItemOnC2SPacket& packet) {
        // Thread safety: This should only be called on the server tick thread
        ASSERT_SERVER_THREAD();
        
        // Validate we're in PLAY state
        if (m_connection.getPhase() != ServerConnection::ConnectionPhase::PLAY) {
            Log::Error("[ServerPlayPacketListener] UseItemOn received outside PLAY state");
            m_connection.SendDisconnect("UseItemOn packet received outside PLAY state");
            return;
        }
        
        // Session is now required, no need to check for null
        Log::Debug("[ServerPlayPacketListener] UseItemOn: hand=%d, pos=(%d,%d,%d), face=%d, seq=%d",
                  packet.hand, packet.blockX, packet.blockY, packet.blockZ, packet.direction, packet.sequence);
        
        // Session handles the packet with correct Minecraft semantics
        m_session.HandleUseItemOn(packet);
    }
    
    void ServerPlayPacketListener::onBlockActionC2S(const Network::BlockActionC2SPacket& packet) {
        Log::Debug("[ServerPlayPacketListener] Received BlockActionC2S - not implemented yet");
        // TODO: Implement when needed
    }
    
    void ServerPlayPacketListener::onPlayerMoveC2S(const Network::PlayerMoveC2SPacket& packet) {
        Log::Debug("[ServerPlayPacketListener] Received PlayerMoveC2S - not implemented yet");
        // TODO: Implement player movement handling
    }
    
    void ServerPlayPacketListener::onChatMessageC2S(const Network::ChatMessageC2SPacket& packet) {
        Log::Debug("[ServerPlayPacketListener] Received ChatMessageC2S: %s", packet.message.c_str());
        // TODO: Implement chat message handling
    }
    
} // namespace Server