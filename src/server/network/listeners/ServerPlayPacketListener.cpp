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

        // Also update the session's keep-alive time to prevent timeout
        m_session.HandleKeepAlive(packet);
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
        m_session.HandleBlockAction(packet);
    }
    
    void ServerPlayPacketListener::onPlayerMoveC2S(const Network::PlayerMoveC2SPacket& packet) {
        m_session.HandlePlayerMove(packet);
    }
    
    void ServerPlayPacketListener::onChatMessageC2S(const Network::ChatMessageC2SPacket& packet) {
        Log::Debug("[ServerPlayPacketListener] Received ChatMessageC2S: %s", packet.message.c_str());
        // TODO: Implement chat message handling
    }

    void ServerPlayPacketListener::onHeldItemChangeC2S(const Network::HeldItemChangeC2SPacket& packet) {
        m_session.HandleHeldItemChange(packet);
    }

    void ServerPlayPacketListener::onChunkBatchAck(float desiredChunksPerTick) {
        // Forward to session for per-player adaptive rate control (Minecraft's PlayerChunkSender)
        m_session.OnChunkBatchAck(desiredChunksPerTick);
    }

} // namespace Server