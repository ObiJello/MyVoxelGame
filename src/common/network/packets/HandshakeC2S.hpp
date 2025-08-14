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
        // Fields match Minecraft wire format exactly
        int32_t protocolVersion;      // VarInt on wire (must be non-negative)
        std::string serverAddress;    // String on wire (VarInt length + UTF-8, max 255)
        uint16_t serverPort;          // Unsigned short on wire (big-endian)
        int32_t nextState;            // VarInt on wire (must be 1 or 2)
        
        // Constructor for creating from network data
        explicit HandshakeC2SPacket(PacketReader& reader) {
            // Read protocol version as VarInt
            protocolVersion = static_cast<int32_t>(reader.ReadVarInt());
            if (protocolVersion < 0) {
                throw std::runtime_error("Invalid protocol version: negative value");
            }
            
            // Read server address with max length 255
            serverAddress = reader.ReadString(255);
            
            // Read server port as big-endian unsigned short
            serverPort = reader.ReadShort();
            
            // Read next state as VarInt and validate
            nextState = static_cast<int32_t>(reader.ReadVarInt());
            if (nextState != static_cast<int32_t>(NextStateWire::STATUS) && 
                nextState != static_cast<int32_t>(NextStateWire::LOGIN)) {
                throw std::runtime_error("Invalid nextState: must be 1 (STATUS) or 2 (LOGIN)");
            }
        }
        
        // Constructor for creating programmatically
        HandshakeC2SPacket(int32_t version, const std::string& addr, uint16_t port, NextStateWire next)
            : protocolVersion(version)
            , serverAddress(addr)
            , serverPort(port)
            , nextState(static_cast<int32_t>(next)) {
            
            if (protocolVersion < 0) {
                throw std::runtime_error("Invalid protocol version: negative value");
            }
            if (serverAddress.length() > 255) {
                throw std::runtime_error("Server address too long: max 255 characters");
            }
        }
        
        // Serialize packet to wire format
        void Serialize(PacketBuffer& buffer) const;
        
        // Get the internal protocol state this handshake transitions to
        ProtocolState GetTargetState() const {
            return static_cast<ProtocolState>(nextState);
        }
        
        PacketId getId() const override { return PacketId::Handshake; }
        
        std::chrono::steady_clock::time_point getTimestamp() const override {
            return std::chrono::steady_clock::now();
        }
        
        // Visitor pattern - apply to listener
        void apply(IPacketListener& listener) override;
    };

} // namespace Network