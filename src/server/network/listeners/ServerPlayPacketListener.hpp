// File: src/server/network/listeners/ServerPlayPacketListener.hpp
#pragma once

#include "common/network/IPacketListener.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"

namespace Server {
    
    class ServerConnection;
    
    class ServerPlayPacketListener : public Network::IPacketListener {
    private:
        ServerConnection& m_connection;
        
    public:
        explicit ServerPlayPacketListener(ServerConnection& connection);
        ~ServerPlayPacketListener() override = default;
        
        // Override from IPacketListener  
        void onKeepAliveResponse(const Network::KeepAliveC2SPacket& packet) override;
        const char* getName() const override { return "ServerPlayPacketListener"; }
        
        // TODO: Add these when we have the packet types defined
        // void onBlockAction(const Network::BlockActionC2SPacket& packet) override;
        // void onPlayerMove(const Network::PlayerMoveC2SPacket& packet) override;
        // void onChatMessage(const Network::ChatMessageC2SPacket& packet) override;
    };
    
} // namespace Server