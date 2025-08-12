// File: src/server/network/listeners/IServerPlayPacketListener.hpp
#pragma once

#include "common/network/IPacketListener.hpp"

namespace Network {
    class PlayerMoveC2SPacket;
    class BlockActionC2SPacket;
    class ChatMessageC2SPacket;
    class KeepAliveC2SPacket;
}

namespace Server {

    // Listener for PLAY protocol state
    class IServerPlayPacketListener : public Network::IPacketListener {
    public:
        virtual ~IServerPlayPacketListener() = default;
        
        // Handle player movement
        virtual void onPlayerMove(const Network::PlayerMoveC2SPacket& packet) = 0;
        
        // Handle block actions (break/place)
        virtual void onBlockAction(const Network::BlockActionC2SPacket& packet) = 0;
        
        // Handle chat messages
        virtual void onChatMessage(const Network::ChatMessageC2SPacket& packet) = 0;
        
        // Handle keep-alive responses
        virtual void onKeepAlive(const Network::KeepAliveC2SPacket& packet) = 0;
        
        const char* getName() const override { return "ServerPlayPacketListener"; }
    };

} // namespace Server