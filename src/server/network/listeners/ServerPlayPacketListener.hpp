// File: src/server/network/listeners/ServerPlayPacketListener.hpp
#pragma once

#include "common/network/IPacketListener.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include "common/network/PacketTypes.hpp"

namespace Server {
    
    class ServerConnection;
    class PlayerSession;
    
    class ServerPlayPacketListener : public Network::IPacketListener {
    private:
        ServerConnection& m_connection;
        PlayerSession& m_session;  // Required session reference
        
    public:
        // Constructor requires session (no longer optional)
        ServerPlayPacketListener(ServerConnection& connection, PlayerSession& session);
        
        ~ServerPlayPacketListener() override = default;
        
        // Override from IPacketListener  
        void onKeepAliveResponse(const Network::KeepAliveC2SPacket& packet) override;
        const char* getName() const override { return "ServerPlayPacketListener"; }
        
        // Block interactions
        void handleUseItemOn(const Network::UseItemOnC2SPacket& packet);  // Minecraft-correct naming
        void onUseItemOnC2S(const Network::UseItemOnC2SPacket& packet) override {
            // Forward to handler method
            handleUseItemOn(packet);
        }
        void onBlockActionC2S(const Network::BlockActionC2SPacket& packet) override;
        
        // Player updates
        void onPlayerMoveC2S(const Network::PlayerMoveC2SPacket& packet) override;
        
        // Chat
        void onChatMessageC2S(const Network::ChatMessageC2SPacket& packet) override;
    };
    
} // namespace Server