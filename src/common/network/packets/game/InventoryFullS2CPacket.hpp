// File: src/common/network/packets/game/InventoryFullS2CPacket.hpp
//
// Server → client: a full snapshot of the OPEN MENU's slots. Sent on join,
// after large mutations, whenever a menu opens or closes, and whenever a click
// arrives predicted against a revision we have moved past.
//
// Mirrors MC ClientboundContainerSetContentPacket (containerId + stateId +
// item list + carried).
//
// ── Menu indices, not inventory indices ─────────────────────────────────────
// `slots` is indexed by MENU slot, which is what every other container packet
// speaks too. For the player's own InventoryMenu the two happen to be the same
// number (Game::Inventory's 46-slot layout matches MC's InventoryMenu order),
// which is why this used to be a fixed 46-entry array of inventory slots. A
// crafting table breaks that: its menu is [result, 3x3 grid, main, hotbar], so
// menu slot 10 is inventory slot 9. `menuType` tells the client which menu
// class to build so it can resolve those indices the same way the server does.
//
// Each slot is a full ItemStack (id + count + component patch) — see
// ItemStackSerialization.hpp for the per-stack wire format (mirrors
// ItemStack.OPTIONAL_STREAM_CODEC).
//
// WIRE CHANGE (containers port): `slots` went from a fixed 46-entry array of
// inventory slots to a length-prefixed list of menu slots, and `menuType` was
// added. Both endpoints ship in one binary — no cross-version shims.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/inventory/MenuType.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct InventoryFullS2CPacket {
        // Monotonic container revision (MC AbstractContainerMenu.stateId).
        // Bumped by the server on every mutation; echoed back by the client on
        // each click so the server can spot a click predicted against stale
        // state.
        uint32_t                     stateId = 0;
        // Which menu this snapshot describes, so the client can build the
        // matching slot list before applying the contents.
        Game::MenuType               menuType = Game::MenuType::Inventory;
        std::vector<Game::ItemStack> slots;
        Game::ItemStack              carried{};
        // Player-inventory concept, not a menu one — the hotbar selection
        // survives whatever menu happens to be open.
        uint8_t                      selectedHotbarSlot = 0;
        // MC AbstractContainerMenu.containerId. The client stores it and stamps
        // it on every click; the server drops clicks carrying a stale id.
        // Bumped when a menu opens or closes, so a click already in flight
        // cannot land on the menu that replaced it.
        uint32_t                     containerId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const InventoryFullS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.stateId);
            buffer.WriteByte(static_cast<uint8_t>(packet.menuType));
            buffer.WriteVarInt(static_cast<uint32_t>(packet.slots.size()));
            for (const auto& slot : packet.slots) WriteItemStack(buffer, slot);
            WriteItemStack(buffer, packet.carried);
            buffer.WriteByte(packet.selectedHotbarSlot);
            buffer.WriteVarInt(packet.containerId);
            return buffer.GetData();
        }

        inline InventoryFullS2CPacket DeserializeInventoryFullS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            InventoryFullS2CPacket packet;
            packet.stateId  = reader.ReadVarInt();
            packet.menuType = static_cast<Game::MenuType>(reader.ReadByte());
            const uint32_t count = reader.ReadVarInt();
            packet.slots.reserve(count);
            for (uint32_t i = 0; i < count; ++i) packet.slots.push_back(ReadItemStack(reader));
            packet.carried            = ReadItemStack(reader);
            packet.selectedHotbarSlot = reader.ReadByte();
            packet.containerId        = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
