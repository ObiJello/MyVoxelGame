// File: src/common/network/packets/game/InventorySetCarriedS2CPacket.hpp
//
// Server → client: cursor item update (item being dragged in inventory UI).
// Mirrors MC ClientboundSetCursorItemPacket (a bare ItemStack).
//
// WIRE CHANGE (components port): moved from (uint32 id + uint8 count) to the
// full ItemStack codec.
//
// ONLY PlayerSession::BroadcastContainerChanges may send this — see the same
// note on InventorySetSlotS2CPacket.hpp for why hand-rolled sends desync the
// remote model and the stateId.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct InventorySetCarriedS2CPacket {
        Game::ItemStack stack{};
        // Container revision this delta brings the client to (MC
        // ClientboundContainerSetSlotPacket.stateId). Carried on the DELTAS
        // too, not just full snapshots, so the client's stateId stays current
        // even when corrections are the only thing it ever receives.
        uint32_t        stateId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventorySetCarriedS2CPacket& packet) {
            PacketBuffer buffer;
            WriteItemStack(buffer, packet.stack);
            buffer.WriteVarInt(packet.stateId);
            return buffer.GetData();
        }

        inline InventorySetCarriedS2CPacket DeserializeInventorySetCarriedS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventorySetCarriedS2CPacket packet;
            packet.stack   = ReadItemStack(reader);
            packet.stateId = reader.HasMore() ? reader.ReadVarInt() : 0u;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
