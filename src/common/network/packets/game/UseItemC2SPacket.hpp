// File: src/common/network/packets/game/UseItemC2SPacket.hpp
//
// Client → server: right-click with NO block target — "use item in air".
// Triggers the server's Item.use dispatch (eat / equip / raise shield / …).
//
// Mirrors MC ServerboundUseItemPacket field-for-field, in MC's write order
// (ServerboundUseItemPacket.java:30-35):
//   writeEnum(hand)      → VarInt   (0 = main hand, 1 = off hand)
//   writeVarInt(sequence)→ VarInt   (shared interaction sequence — same
//                                    counter as UseItemOnC2S / dig)
//   writeFloat(yRot)     → float    (player yaw at click time; server snaps
//                                    rotation to it, ServerGamePacketListenerImpl.java:1338-1342)
//   writeFloat(xRot)     → float    (player pitch at click time)
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct UseItemC2SPacket {
        uint32_t hand     = 0;     // 0 = main hand, 1 = off hand
        uint32_t sequence = 0;     // monotonic interaction id
        float    yRot     = 0.0f;  // yaw (degrees)
        float    xRot     = 0.0f;  // pitch (degrees)
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const UseItemC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.hand);      // :31
            buffer.WriteVarInt(packet.sequence);  // :32
            buffer.WriteFloat(packet.yRot);       // :33
            buffer.WriteFloat(packet.xRot);       // :34
            return buffer.GetData();
        }

        inline UseItemC2SPacket DeserializeUseItemC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            UseItemC2SPacket packet;
            packet.hand     = reader.ReadVarInt();
            packet.sequence = reader.ReadVarInt();
            packet.yRot     = reader.ReadFloat();
            packet.xRot     = reader.ReadFloat();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
