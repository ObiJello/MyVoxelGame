// File: src/common/network/packets/HandshakeC2S.hpp
#pragma once

#include "common/network/IPacket.hpp"
#include "common/network/IPacketListener.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/ProtocolTypes.hpp"

#include <stdexcept>

namespace Server {
    class IHandshakePacketListener;
}

namespace Network {

    // THE protocol version, for both ends. Bump it whenever the wire format
    // changes in a way an older peer cannot read.
    //
    // 754 was MC 1.16.5's number, inherited when the handshake was written.
    // 755 is the first version of OUR protocol that diverges from it: the
    // chunk packet lost its section bitmask and became positional (every
    // section, ascending Y, no mask — matching modern MC, which dropped the
    // mask when it moved to 3D biomes). A 754 peer reading a 755 chunk stream
    // consumes the first section's data as a VarInt bitmask and every section
    // after it lands at the wrong Y.
    //
    // That failure is silent and looks like corrupt terrain, which is why the
    // check on this is a DISCONNECT and not a warning. It used to log and
    // carry on "for testing", which was survivable only while the two ends
    // could not actually disagree about anything.
    inline constexpr int32_t kProtocolVersion = 755;

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