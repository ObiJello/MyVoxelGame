// File: src/server/network/listeners/ServerPlayPacketListener.hpp
#pragma once

#include "IServerPlayPacketListener.hpp"

namespace Server {
    
    class ServerConnection;
    
    class ServerPlayPacketListener : public IServerPlayPacketListener {
    private:
        ServerConnection& m_connection;
        
    public:
        explicit ServerPlayPacketListener(ServerConnection& connection);
        ~ServerPlayPacketListener() override = default;
        
        // IServerPlayPacketListener implementation
        void onBlockAction(const Network::BlockActionC2SPacket& packet) override;
        void onPlayerMove(const Network::PlayerMoveC2SPacket& packet) override;
        void onChatMessage(const Network::ChatMessageC2SPacket& packet) override;
        void onKeepAlive(const Network::KeepAliveC2SPacket& packet) override;
    };
    
} // namespace Server