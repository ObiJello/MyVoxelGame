// File: src/common/network/packets/game/InventoryFullS2CPacket.hpp
//
// Server → client: full 46-slot snapshot. Sent on join and after large
// mutations. ItemID is uint32_t — block items use IDs 1..(BlockID::Count-1),
// pure items >= 0x10000.
//
// Mirrors MC ClientboundContainerSetContentPacket (the inventory variant).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace Network {

    struct InventoryFullS2CPacket {
        std::array<uint32_t, 46> itemIds;
        std::array<uint8_t,  46> counts;
        uint32_t carriedItemId      = 0;
        uint8_t  carriedCount       = 0;
        uint8_t  selectedHotbarSlot = 0;

        InventoryFullS2CPacket() { itemIds.fill(0); counts.fill(0); }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventoryFullS2CPacket& packet) {
            PacketBuffer buffer;
            for (int i = 0; i < 46; ++i) buffer.WriteInt(packet.itemIds[i]);
            for (int i = 0; i < 46; ++i) buffer.WriteByte(packet.counts[i]);
            buffer.WriteInt(packet.carriedItemId);
            buffer.WriteByte(packet.carriedCount);
            buffer.WriteByte(packet.selectedHotbarSlot);
            return buffer.GetData();
        }

        inline InventoryFullS2CPacket DeserializeInventoryFullS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventoryFullS2CPacket packet;
            for (int i = 0; i < 46; ++i) packet.itemIds[i] = reader.ReadInt();
            for (int i = 0; i < 46; ++i) packet.counts[i]  = reader.ReadByte();
            packet.carriedItemId      = reader.ReadInt();
            packet.carriedCount       = reader.ReadByte();
            packet.selectedHotbarSlot = reader.ReadByte();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
