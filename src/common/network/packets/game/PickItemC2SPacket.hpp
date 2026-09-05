// File: src/common/network/packets/game/PickItemC2SPacket.hpp
//
// Pick block / pick entity (the P key). Mirrors MC's ServerboundPickItemFrom
// BlockPacket (pos) and ServerboundPickItemFromEntityPacket (id) in one
// packet: the client says WHAT it aimed at and the server resolves the item
// and rearranges the inventory (ServerGamePacketListenerImpl.tryPickItem),
// then tells the client the new held slot with SetHeldSlotS2C and the slot
// changes through the normal inventory diff.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PickItemC2SPacket {
        enum class Kind : uint8_t { Block = 0, Entity = 1 };
        Kind    kind = Kind::Block;
        int32_t x = 0, y = 0, z = 0;   // Block: the block's position
        int32_t entityId = 0;          // Entity: the entity id
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PickItemC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(static_cast<uint8_t>(packet.kind));
            buffer.WriteInt(packet.x);
            buffer.WriteInt(packet.y);
            buffer.WriteInt(packet.z);
            buffer.WriteInt(packet.entityId);
            return buffer.GetData();
        }

        inline PickItemC2SPacket DeserializePickItemC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PickItemC2SPacket packet;
            packet.kind     = static_cast<PickItemC2SPacket::Kind>(reader.ReadByte());
            packet.x        = reader.ReadInt();
            packet.y        = reader.ReadInt();
            packet.z        = reader.ReadInt();
            packet.entityId = reader.ReadInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
