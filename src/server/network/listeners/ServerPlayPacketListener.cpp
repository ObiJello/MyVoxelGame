// File: src/server/network/listeners/ServerPlayPacketListener.cpp
#include "ServerPlayPacketListener.hpp"
#include "../ServerConnection.hpp"
#include "../../session/PlayerSession.hpp"
#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include "common/network/PacketTypes.hpp"
#include "server/IntegratedServer.hpp"

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
        // A block may have asked for its container UI during that dispatch
        // (IUsePlayer::OpenMenu). Doing it here rather than inside the dispatch
        // keeps the request out of the many early-return paths, and still puts
        // the screen up on the same round trip as the interaction ack.
        m_session.FlushPendingMenuOpen();
        // Same deal for a campfire that was handed food during the dispatch
        // (IUsePlayer::PlaceCampfireFood) — the block entity lookup needs the
        // world, which the dispatch doesn't carry.
        m_session.FlushPendingCampfireFood();
    }
    
    void ServerPlayPacketListener::handleUseItem(const Network::UseItemC2SPacket& packet) {
        // Mirrors ServerGamePacketListenerImpl.handleUseItem's thread + phase
        // guards (ServerGamePacketListenerImpl.java:1329-1331).
        ASSERT_SERVER_THREAD();

        if (m_connection.getPhase() != ServerConnection::ConnectionPhase::PLAY) {
            Log::Error("[ServerPlayPacketListener] UseItem received outside PLAY state");
            m_connection.SendDisconnect("UseItem packet received outside PLAY state");
            return;
        }

        Log::Debug("[ServerPlayPacketListener] UseItem: hand=%u, seq=%u, yRot=%.1f, xRot=%.1f",
                   packet.hand, packet.sequence, packet.yRot, packet.xRot);

        m_session.HandleUseItem(packet);
    }

    void ServerPlayPacketListener::handlePlayerAction(const Network::PlayerActionC2SPacket& packet) {
        // Mirrors ServerGamePacketListenerImpl.handlePlayerAction's thread +
        // phase guards (ServerGamePacketListenerImpl.java:1191-1193).
        ASSERT_SERVER_THREAD();

        if (m_connection.getPhase() != ServerConnection::ConnectionPhase::PLAY) {
            Log::Error("[ServerPlayPacketListener] PlayerAction received outside PLAY state");
            m_connection.SendDisconnect("PlayerAction packet received outside PLAY state");
            return;
        }

        m_session.HandlePlayerAction(packet);
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

    void ServerPlayPacketListener::onInventoryClickC2S(const Network::InventoryClickC2SPacket& packet) {
        m_session.HandleInventoryClick(packet);
    }

    void ServerPlayPacketListener::onPlayerAbilitiesC2S(const Network::PlayerAbilitiesC2SPacket& packet) {
        m_session.HandlePlayerAbilities(packet);
    }

    void ServerPlayPacketListener::onInteractC2S(const Network::InteractC2SPacket& packet) {
        // Straight to the server rather than through the session: the mob
        // system is owned by IntegratedServer, and PlayerSession has no reason
        // to know about it.
        if (!Server::g_integratedServer) return;
        Server::g_integratedServer->HandleInteract(
            m_connection.GetConnectionId(), packet.entityId,
            packet.action == Network::InteractC2SPacket::Action::Attack,
            packet.sprinting);
    }

    void ServerPlayPacketListener::onInventoryCloseC2S(const Network::InventoryCloseC2SPacket& packet) {
        m_session.HandleInventoryClose(packet);
    }

    void ServerPlayPacketListener::onChunkBatchAck(float desiredChunksPerTick) {
        // Forward to session for per-player adaptive rate control (Minecraft's PlayerChunkSender)
        m_session.OnChunkBatchAck(desiredChunksPerTick);
    }

} // namespace Server