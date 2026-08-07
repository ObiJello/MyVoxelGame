// File: src/common/network/packets/game/InventoryFullS2CPacket.hpp
//
// Server → client: full 46-slot snapshot. Sent on join and after large
// mutations. Each slot is a full ItemStack (id + count + component patch) —
// see ItemStackSerialization.hpp for the per-stack wire format (mirrors
// ItemStack.OPTIONAL_STREAM_CODEC).
//
// Mirrors MC ClientboundContainerSetContentPacket (the inventory variant).
// MC writes containerId + stateId + a length-prefixed list + carried; we have
// exactly one container (the player inventory) and a fixed 46-slot layout, so
// containerId/list-length are omitted (documented deviation). `stateId` IS
// carried — it is what lets the client tell a fresh authoritative snapshot
// from a stale one that overtook its own prediction.
//
// WIRE CHANGE (components port): slots moved from (uint32 id + uint8 count)
// arrays to full ItemStack codec. Both endpoints ship in one binary — no
// cross-version compatibility shims.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace Network {

    struct InventoryFullS2CPacket {
        // Monotonic container revision (MC AbstractContainerMenu.stateId).
        // Bumped by the server on every inventory mutation; echoed back by the
        // client on each click so the server can spot a click that was
        // predicted against stale state.
        uint32_t                        stateId = 0;
        std::array<Game::ItemStack, 46> slots{};
        Game::ItemStack                 carried{};
        uint8_t                         selectedHotbarSlot = 0;
        // Which menu this snapshot describes (MC
        // AbstractContainerMenu.containerId). The client stores it and stamps it
        // on every click; the server drops clicks carrying a stale id. Bumped
        // when a menu closes, so a click already in flight cannot land on the
        // menu that replaced it.
        uint32_t                        containerId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventoryFullS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.stateId);
            for (int i = 0; i < 46; ++i) WriteItemStack(buffer, packet.slots[i]);
            WriteItemStack(buffer, packet.carried);
            buffer.WriteByte(packet.selectedHotbarSlot);
            // Tail-appended so an older serialized packet still decodes.
            buffer.WriteVarInt(packet.containerId);
            return buffer.GetData();
        }

        inline InventoryFullS2CPacket DeserializeInventoryFullS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventoryFullS2CPacket packet;
            packet.stateId = reader.ReadVarInt();
            for (int i = 0; i < 46; ++i) packet.slots[i] = ReadItemStack(reader);
            packet.carried            = ReadItemStack(reader);
            packet.selectedHotbarSlot = reader.ReadByte();
            packet.containerId        = reader.HasMore() ? reader.ReadVarInt() : 0u;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
