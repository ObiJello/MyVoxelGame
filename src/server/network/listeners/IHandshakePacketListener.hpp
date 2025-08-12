// File: src/server/network/listeners/IHandshakePacketListener.hpp
#pragma once

#include "common/network/IPacketListener.hpp"

namespace Network {
    class HandshakeC2SPacket;
}

namespace Server {

    // Listener for HANDSHAKING protocol state
    class IHandshakePacketListener : public Network::IPacketListener {
    public:
        virtual ~IHandshakePacketListener() = default;
        
        // Handle handshake packet
        virtual void onHandshake(const Network::HandshakeC2SPacket& packet) = 0;
        
        const char* getName() const override { return "HandshakePacketListener"; }
    };

} // namespace Server