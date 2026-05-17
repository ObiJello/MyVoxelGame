// File: src/common/network/packets/game/InventorySetCarriedS2CPacket.hpp
//
// Server → client: cursor item update (item being dragged in inventory UI).
// Mirrors MC ClientboundContainerSetCarriedItemPacket equivalent for the
// player's inventory cursor.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct InventorySetCarriedS2CPacket {
        uint32_t itemId = 0;
        uint8_t  count  = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventorySetCarriedS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteInt(packet.itemId);
            buffer.WriteByte(packet.count);
            return buffer.GetData();
        }

        inline InventorySetCarriedS2CPacket DeserializeInventorySetCarriedS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventorySetCarriedS2CPacket packet;
            packet.itemId = reader.ReadInt();
            packet.count  = reader.ReadByte();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
