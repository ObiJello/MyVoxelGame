// File: src/common/network/packets/game/InventorySetSlotS2CPacket.hpp
//
// Server → client: single-slot delta. Mirrors MC ClientboundContainerSetSlotPacket
// (containerId + stateId omitted — single fixed container; slot short + full
// ItemStack kept in MC's order).
//
// WIRE CHANGE (components port): stack moved from (uint32 id + uint8 count) to
// the full ItemStack codec.
//
// ── ONLY PlayerSession::BroadcastContainerChanges MAY SEND THIS ──────────────
// MC has exactly one outbound container path: mutate the container, and
// ServerPlayer.doTick's containerMenu.broadcastChanges() diffs it against the
// remote model. Server code that hand-rolls this packet skips two things the
// diff does — updating m_remoteSlots (so the model goes stale and the slot is
// re-sent forever) and stamping a real stateId (a default 0 makes the client
// echo 0, which disables the click staleness guard entirely). Four such sites
// existed and all four were desync sources. Mutate the inventory and let the
// per-tick diff do the sending; if you need it sooner, call
// BroadcastContainerChanges() directly (see PlayerSession::ResyncAndAck for the
// one case that must also call InvalidateRemoteSlot first).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct InventorySetSlotS2CPacket {
        int16_t         slotIndex = 0; // 0..45 (MC writes a short here too)
        Game::ItemStack stack{};
        // Container revision this delta brings the client to (MC
        // ClientboundContainerSetSlotPacket.stateId). Carried on the DELTAS
        // too, not just full snapshots, so the client's stateId stays current
        // even when corrections are the only thing it ever receives.
        uint32_t        stateId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventorySetSlotS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteShort(static_cast<uint16_t>(packet.slotIndex));
            WriteItemStack(buffer, packet.stack);
            buffer.WriteVarInt(packet.stateId);
            return buffer.GetData();
        }

        inline InventorySetSlotS2CPacket DeserializeInventorySetSlotS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventorySetSlotS2CPacket packet;
            packet.slotIndex = static_cast<int16_t>(reader.ReadShort());
            packet.stack     = ReadItemStack(reader);
            packet.stateId   = reader.HasMore() ? reader.ReadVarInt() : 0u;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
