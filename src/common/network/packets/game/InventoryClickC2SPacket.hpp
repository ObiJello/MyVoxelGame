// File: src/common/network/packets/game/InventoryClickC2SPacket.hpp
//
// Mirrors MC ServerboundContainerClickPacket — single inventory action.
// `action` is a ContainerInput enum (PICKUP / QUICK_MOVE / SWAP / CLONE /
// THROW / QUICK_CRAFT / PICKUP_ALL / CREATIVE_DESTROY_ALL).
// `slotIndex` is 0..45 OR an InventorySlotSentinel:: value (OUTSIDE,
// CREATIVE_GRID).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "../common/PacketCommon.hpp"  // ContainerInput, InventorySlotSentinel
#include <cstdint>
#include <vector>

namespace Network {

    struct InventoryClickC2SPacket {
        int16_t  slotIndex      = 0;  // 0..45 OR InventorySlotSentinel::*
        uint8_t  button         = 0;  // semantics depend on action
        uint8_t  action         = 0;  // ContainerInput as uint8_t
        uint8_t  flags          = 0;  // reserved (e.g. shift redundancy bits)
        uint32_t creativeItemId = 0;  // only meaningful when slotIndex == CREATIVE_GRID
        // Full stack for creative-source clicks, INCLUDING per-stack components
        // (an enchanted-book variant from the search grid carries its
        // STORED_ENCHANTMENTS here). Mirrors MC's
        // ServerboundSetCreativeModeSlotPacket carrying a real ItemStack. When
        // empty, the server falls back to creativeItemId + item defaults.
        Game::ItemStack creativeStack{};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventoryClickC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteShort(static_cast<uint16_t>(packet.slotIndex));
            buffer.WriteByte(packet.button);
            buffer.WriteByte(packet.action);
            buffer.WriteByte(packet.flags);
            buffer.WriteInt(packet.creativeItemId);
            WriteItemStack(buffer, packet.creativeStack);
            return buffer.GetData();
        }

        inline InventoryClickC2SPacket DeserializeInventoryClickC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventoryClickC2SPacket packet;
            packet.slotIndex      = static_cast<int16_t>(reader.ReadShort());
            packet.button         = reader.ReadByte();
            packet.action         = reader.ReadByte();
            packet.flags          = reader.ReadByte();
            packet.creativeItemId = reader.ReadInt();
            // creativeStack — appended at end so older serialized packets
            // (without it) cleanly default to empty (same back-compat trick as
            // UseItemOnC2SPacket.altInteract).
            packet.creativeStack  = reader.HasMore() ? ReadItemStack(reader)
                                                     : Game::ItemStack{};
            return packet;
        }

    } // namespace Serialization

} // namespace Network
