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
#include <utility>
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
        // Container revision the client predicted against (MC
        // ServerboundContainerClickPacket.stateId). A mismatch against the
        // server's current id means the prediction was built on stale state.
        uint32_t stateId = 0;

        // Which menu the client believes it is clicking (MC
        // ServerboundContainerClickPacket.containerId, checked as the very
        // first thing in handleContainerClick). The server bumps its id when a
        // menu closes or is replaced, so a click aimed at a menu that is no
        // longer open is dropped instead of being applied to whatever took its
        // place. 0 means "unknown" — accepted, for clients that predate the
        // field.
        uint32_t containerId = 0;

        // The client's PREDICTED outcome of this click — mirrors MC
        // ServerboundContainerClickPacket's changedSlots + carriedItem.
        // The server adopts these as its model of what the client now
        // believes, then sends corrections only where that model disagrees
        // with the truth. A correct prediction therefore costs zero packets,
        // and a slot the client wrongly wrote is still corrected because the
        // client itself reported writing it.
        std::vector<std::pair<uint8_t, Game::ItemStack>> predictedSlots;
        Game::ItemStack predictedCarried{};
        bool            hasPrediction = false;
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
            buffer.WriteVarInt(packet.stateId);
            buffer.WriteByte(packet.hasPrediction ? 1 : 0);
            if (packet.hasPrediction) {
                buffer.WriteVarInt(static_cast<uint32_t>(packet.predictedSlots.size()));
                for (const auto& [slot, stack] : packet.predictedSlots) {
                    buffer.WriteByte(slot);
                    WriteItemStack(buffer, stack);
                }
                WriteItemStack(buffer, packet.predictedCarried);
            }
            // containerId is written LAST, after the variable-length prediction
            // block, so the same tail-append back-compat rule applies to it.
            buffer.WriteVarInt(packet.containerId);
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
            packet.stateId        = reader.HasMore() ? reader.ReadVarInt() : 0u;
            packet.hasPrediction  = reader.HasMore() && reader.ReadByte() != 0;
            if (packet.hasPrediction) {
                const uint32_t n = reader.ReadVarInt();
                packet.predictedSlots.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    const uint8_t slot = reader.ReadByte();
                    packet.predictedSlots.emplace_back(slot, ReadItemStack(reader));
                }
                packet.predictedCarried = ReadItemStack(reader);
            }
            packet.containerId = reader.HasMore() ? reader.ReadVarInt() : 0u;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
