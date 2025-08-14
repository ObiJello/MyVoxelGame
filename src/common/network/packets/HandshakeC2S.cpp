// File: src/common/network/packets/HandshakeC2S.cpp
#include "HandshakeC2S.hpp"
#include "common/network/IPacketListener.hpp"

namespace Network {

    void HandshakeC2SPacket::apply(IPacketListener& listener) {
        // Direct virtual call - no dynamic_cast needed!
        // This fixes the Windows RTTI issue
        listener.onHandshake(*this);
    }

    void HandshakeC2SPacket::Serialize(PacketBuffer& buffer) const {
        // Validate before serializing
        if (protocolVersion < 0) {
            throw std::runtime_error("Cannot serialize negative protocol version");
        }
        if (serverAddress.length() > 255) {
            throw std::runtime_error("Server address too long for serialization: max 255 characters");
        }
        if (nextState != static_cast<int32_t>(NextStateWire::STATUS) && 
            nextState != static_cast<int32_t>(NextStateWire::LOGIN)) {
            throw std::runtime_error("Invalid nextState for serialization: must be 1 or 2");
        }
        
        // Write fields in Minecraft wire order
        buffer.WriteVarInt(static_cast<uint32_t>(protocolVersion));  // VarInt
        buffer.WriteString(serverAddress);                           // String (VarInt length + UTF-8)
        buffer.WriteShort(serverPort);                              // Unsigned short (big-endian)
        buffer.WriteVarInt(static_cast<uint32_t>(nextState));       // VarInt (1 or 2)
    }

} // namespace Network