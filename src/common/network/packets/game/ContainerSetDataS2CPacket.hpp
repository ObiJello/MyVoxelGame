// File: src/common/network/packets/game/ContainerSetDataS2CPacket.hpp
//
// Server → client: one entry of the open menu's ContainerData changed. Mirrors
// MC ClientboundContainerSetDataPacket (containerId + id + value; we keep the
// containerId so a delta for a menu the client already closed is dropped rather
// than applied to whatever replaced it).
//
// This is how a furnace's flame and progress arrow animate: the block entity
// ticks its counters, PlayerSession's per-tick diff notices, and one of these
// goes out per changed index. Values are ints because that is what MC's
// ContainerData is — cook time, burn time, fuel level, enchantment cost.
//
// ── ONLY PlayerSession::BroadcastContainerChanges MAY SEND THIS ──────────────
// Same rule as InventorySetSlotS2CPacket, for the same reason: the diff owns
// m_remoteData, and a hand-rolled send leaves that model stale so the value is
// either re-sent forever or never again.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct ContainerSetDataS2CPacket {
        uint32_t containerId = 0;
        uint16_t id          = 0;   // index into the menu's ContainerData
        int32_t  value       = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ContainerSetDataS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.containerId);
            buffer.WriteShort(packet.id);
            // Signed: MC's data slots are plain ints and some go negative
            // (an enchanting table writes -1 for "no offer in this row").
            buffer.WriteInt(static_cast<uint32_t>(packet.value));
            return buffer.GetData();
        }

        inline ContainerSetDataS2CPacket DeserializeContainerSetDataS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            ContainerSetDataS2CPacket packet;
            packet.containerId = reader.ReadVarInt();
            packet.id          = reader.ReadShort();
            packet.value       = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network
