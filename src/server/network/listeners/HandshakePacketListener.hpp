// File: src/server/network/listeners/HandshakePacketListener.hpp
#pragma once

#include "common/network/IPacketListener.hpp"
#include "common/network/packets/HandshakeC2S.hpp"

namespace Server {

    class ServerConnection;

    class HandshakePacketListener : public Network::IPacketListener {
    private:
        ServerConnection& m_connection;
        
    public:
        explicit HandshakePacketListener(ServerConnection& connection);
        ~HandshakePacketListener() override = default;
        
        // Handle handshake packet
        void onHandshake(const Network::HandshakeC2SPacket& packet) override;
        
        // Handle disconnect
        void onDisconnect(const std::string& reason) override;
        
        // Get listener name for debugging
        const char* getName() const override { return "HandshakePacketListener"; }
    };

} // namespace Server