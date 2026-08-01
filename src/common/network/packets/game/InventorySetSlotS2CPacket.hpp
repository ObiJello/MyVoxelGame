// File: src/common/network/packets/game/InventorySetSlotS2CPacket.hpp
//
// Server → client: single-slot delta. Mirrors MC ClientboundContainerSetSlotPacket
// (containerId + stateId omitted — single fixed container; slot short + full
// ItemStack kept in MC's order).
//
// WIRE CHANGE (components port): stack moved from (uint32 id + uint8 count) to
// the full ItemStack codec.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct InventorySetSlotS2CPacket {
        int16_t         slotIndex = 0; // 0..45 (MC writes a short here too)
        Game::ItemStack stack{};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventorySetSlotS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteShort(static_cast<uint16_t>(packet.slotIndex));
            WriteItemStack(buffer, packet.stack);
            return buffer.GetData();
        }

        inline InventorySetSlotS2CPacket DeserializeInventorySetSlotS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventorySetSlotS2CPacket packet;
            packet.slotIndex = static_cast<int16_t>(reader.ReadShort());
            packet.stack     = ReadItemStack(reader);
            return packet;
        }

    } // namespace Serialization

} // namespace Network
