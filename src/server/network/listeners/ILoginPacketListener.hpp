// File: src/server/network/listeners/ILoginPacketListener.hpp
#pragma once

#include "common/network/IPacketListener.hpp"

namespace Network {
    class LoginStartC2SPacket;
    class EncryptionResponseC2SPacket;
}

namespace Server {

    // Listener for LOGIN protocol state
    class ILoginPacketListener : public Network::IPacketListener {
    public:
        virtual ~ILoginPacketListener() = default;
        
        // Handle login start packet
        virtual void onLoginStart(const Network::LoginStartC2SPacket& packet) = 0;
        
        // Handle encryption response (for online mode)
        virtual void onEncryptionResponse(const Network::EncryptionResponseC2SPacket& packet) {}
        
        const char* getName() const override { return "LoginPacketListener"; }
    };

} // namespace Server