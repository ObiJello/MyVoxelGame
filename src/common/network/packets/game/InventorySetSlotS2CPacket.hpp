// File: src/common/network/packets/game/InventorySetSlotS2CPacket.hpp
//
// Server → client: single-slot delta. Mirrors MC ClientboundContainerSetSlotPacket.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct InventorySetSlotS2CPacket {
        uint8_t  slotIndex = 0; // 0..45
        uint32_t itemId    = 0;
        uint8_t  count     = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventorySetSlotS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.slotIndex);
            buffer.WriteInt(packet.itemId);
            buffer.WriteByte(packet.count);
            return buffer.GetData();
        }

        inline InventorySetSlotS2CPacket DeserializeInventorySetSlotS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventorySetSlotS2CPacket packet;
            packet.slotIndex = reader.ReadByte();
            packet.itemId    = reader.ReadInt();
            packet.count     = reader.ReadByte();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
