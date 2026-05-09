// File: src/server/inventory/InventoryClickHandler.hpp
// Server-authoritative inventory click dispatch.
// Mirrors net.minecraft.world.inventory.AbstractContainerMenu.doClick().
#pragma once

#include "common/network/PacketTypes.hpp"
#include "common/entity/Inventory.hpp"
#include <vector>
#include <cstdint>

namespace Server {

    class ServerPlayer;

    struct InventoryClickResult {
        std::vector<uint8_t> changedSlots;       // unique slot indices to rebroadcast
        bool                 carriedChanged = false;
        Game::InventorySlot  droppedItem{};      // populated for THROW (entity spawn TODO)
    };

    class InventoryClickHandler {
    public:
        // Apply one click to the player's authoritative inventory and cursor.
        static InventoryClickResult Handle(
            ServerPlayer& player,
            const Network::InventoryClickC2SPacket& click);

    private:
        static InventoryClickResult HandlePickup    (ServerPlayer&, int16_t slotIndex, uint8_t button);
        static InventoryClickResult HandleQuickMove (ServerPlayer&, int16_t slotIndex, uint8_t button);
        static InventoryClickResult HandleSwap      (ServerPlayer&, int16_t slotIndex, uint8_t button);
        static InventoryClickResult HandleClone     (ServerPlayer&, int16_t slotIndex);
        static InventoryClickResult HandleThrow     (ServerPlayer&, int16_t slotIndex, uint8_t button);
        static InventoryClickResult HandleQuickCraft(ServerPlayer&, int16_t slotIndex, uint8_t button);
        static InventoryClickResult HandlePickupAll (ServerPlayer&, int16_t slotIndex);
        // Creative shift-click on the destroy_item slot — clears every player slot.
        static InventoryClickResult HandleCreativeDestroyAll(ServerPlayer&);

        // Creative search-grid clicks (slotIndex == InventorySlotSentinel::CREATIVE_GRID).
        static InventoryClickResult HandleCreativePickup   (ServerPlayer&, uint32_t itemId, uint8_t button);
        static InventoryClickResult HandleCreativeQuickMove(ServerPlayer&, uint32_t itemId);

        // Mark a unique slot as changed.
        static void MarkChanged(InventoryClickResult& r, uint8_t slot);

        // Try to merge `src` into `dst` (same blockId, capped at MAX_STACK_SIZE).
        // Returns the number of items moved.
        static int MergeInto(Game::InventorySlot& dst, Game::InventorySlot& src);

        // Quick-move helper: distribute `slot` (cleared on output) across [begin, end).
        // Returns true if anything was moved.
        static bool MoveStackToRegion(Game::Inventory& inv,
                                      int sourceIndex,
                                      int rangeBegin, int rangeEnd,
                                      InventoryClickResult& result);

        // Visual-only slots: armor + crafting. Never accept inserts from user clicks.
        static bool IsRestrictedInsertSlot(int16_t slotIndex);
    };

} // namespace Server
