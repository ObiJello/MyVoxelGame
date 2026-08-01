// File: src/common/network/packets/game/InventorySetCarriedS2CPacket.hpp
//
// Server → client: cursor item update (item being dragged in inventory UI).
// Mirrors MC ClientboundSetCursorItemPacket (a bare ItemStack).
//
// WIRE CHANGE (components port): moved from (uint32 id + uint8 count) to the
// full ItemStack codec.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct InventorySetCarriedS2CPacket {
        Game::ItemStack stack{};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventorySetCarriedS2CPacket& packet) {
            PacketBuffer buffer;
            WriteItemStack(buffer, packet.stack);
            return buffer.GetData();
        }

        inline InventorySetCarriedS2CPacket DeserializeInventorySetCarriedS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventorySetCarriedS2CPacket packet;
            packet.stack = ReadItemStack(reader);
            return packet;
        }

    } // namespace Serialization

} // namespace Network
