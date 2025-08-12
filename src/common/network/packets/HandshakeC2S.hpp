// File: src/common/network/packets/HandshakeC2S.hpp
#pragma once

#include "common/network/IPacket.hpp"
#include "common/network/IPacketListener.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/ProtocolTypes.hpp"

namespace Server {
    class IHandshakePacketListener;
}

namespace Network {

    class HandshakeC2SPacket : public IC2SPacket {
    public:
        int protocolVersion;
        std::string serverAddress;
        uint16_t serverPort;
        ProtocolState nextState;
        
        // Constructor for creating from network data
        explicit HandshakeC2SPacket(PacketReader& reader) {
            protocolVersion = reader.ReadVarInt();
            serverAddress = reader.ReadString();
            serverPort = reader.ReadShort();
            int next = reader.ReadVarInt();
            nextState = static_cast<ProtocolState>(next);
        }
        
        // Constructor for creating programmatically
        HandshakeC2SPacket(int version, const std::string& addr, uint16_t port, ProtocolState next)
            : protocolVersion(version)
            , serverAddress(addr)
            , serverPort(port)
            , nextState(next) {}
        
        PacketId getId() const override { return PacketId::Handshake; }
        
        std::chrono::steady_clock::time_point getTimestamp() const override {
            return std::chrono::steady_clock::now();
        }
        
        // Visitor pattern - apply to listener
        void apply(IPacketListener& listener) override;
    };

} // namespace Network