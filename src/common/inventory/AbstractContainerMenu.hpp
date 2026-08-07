// File: src/common/inventory/AbstractContainerMenu.hpp
//
// Mirrors net.minecraft.world.inventory.AbstractContainerMenu — an ordered list
// of Slots plus the cursor ("carried") stack and the click dispatch that
// operates on them.
//
// This runs UNCHANGED on both sides. MC does the same: doClick is common code
// that the client executes against its local menu for instant feedback and the
// server re-executes authoritatively, correcting only where the two disagree.
// That is why moving items in an MC inventory feels instant even on a laggy
// server, and it is why every behavioural rule must live here rather than in
// InventoryScreen (client) or PlayerSession (server).
//
// Replaces the older ContainerHost + ContainerClick pair, which indexed
// Game::Inventory directly and so could only ever describe the player's own
// 46 slots.
//
// ── Index spaces ────────────────────────────────────────────────────────────
// `slotIndex` throughout this class — and in ContainerClickResult::changedSlots
// and on the wire — is a MENU index. For InventoryMenu (the only menu today)
// menu index == player-inventory index, because Game::Inventory's layout
// already matches MC's InventoryMenu slot order. PlayerSession's remote-slot
// diff relies on that identity: it walks inventory indices 0..45. When a menu
// that spans two containers arrives (a chest), that diff must be changed to
// walk menu slots instead — that is the one place the equivalence is assumed.
#pragma once

#include "Slot.hpp"
#include "common/entity/Inventory.hpp"
#include "common/network/PacketTypes.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace Game {

    struct ContainerClickResult {
        std::vector<uint8_t> changedSlots;    // unique MENU indices to rebroadcast
        bool                 carriedChanged = false;
        ItemStack            droppedItem{};   // populated for THROW (entity spawn TODO)
    };

    class AbstractContainerMenu {
    public:
        virtual ~AbstractContainerMenu() = default;

        // ── Slots ─────────────────────────────────────────────────────────
        int   SlotCount() const { return static_cast<int>(m_slots.size()); }
        Slot& GetSlot(int index) { return *m_slots[index]; }
        const Slot& GetSlot(int index) const { return *m_slots[index]; }
        bool  IsValidSlotIndex(int index) const {
            return index >= 0 && index < SlotCount();
        }

        // ── Cursor ────────────────────────────────────────────────────────
        ItemStack&       getCarried()       { return m_carried; }
        const ItemStack& getCarried() const { return m_carried; }
        void setCarried(const ItemStack& s) { m_carried = s; }

        // The player inventory this menu belongs to. MC's doClick takes the
        // Player and reads player.getInventory() for the SWAP hotbar source;
        // we hold the pointer instead. Never null for a constructed menu.
        Inventory&       getInventory()       { return *m_playerInventory; }
        const Inventory& getInventory() const { return *m_playerInventory; }

        // Drives the creative-only click paths (CLONE, creative grid).
        bool creative = false;

        // MC AbstractContainerMenu.containerId. Echoed by the client on every
        // click; a mismatch means the click targets a menu that has since been
        // replaced or closed, and is dropped. Bumped on close.
        uint32_t containerId = 1;

        // ── Click dispatch ────────────────────────────────────────────────
        // Apply one click. Same call on both sides: the client runs it to
        // predict, the server to decide.
        ContainerClickResult DoClick(const Network::InventoryClickC2SPacket& click);

        // MC AbstractContainerMenu.quickMoveStack — where does a shift-click on
        // `slotIndex` send its contents? Menu-specific, hence abstract.
        virtual void QuickMoveStack(int slotIndex, ContainerClickResult& result) = 0;

    protected:
        explicit AbstractContainerMenu(Inventory* playerInventory)
            : m_playerInventory(playerInventory) {}

        // Append a slot; assigns its menu index. Mirrors MC addSlot.
        Slot& AddSlot(std::unique_ptr<Slot> slot);

        // MC AbstractContainerMenu.moveItemStackTo — merge `stack` into
        // matching stacks in [begin, end), then into the first empty slot that
        // accepts it. Shrinks `stack` by whatever moved. Returns true if
        // anything moved. Every touched slot is recorded on `result`.
        bool MoveItemStackTo(ItemStack& stack, int begin, int end, bool backwards,
                             ContainerClickResult& result);

        // Creative-only paths. These are our extension rather than MC's (MC
        // routes creative through ServerboundSetCreativeModeSlotPacket and a
        // client-local ItemPickerMenu), so they are virtual: the base gives a
        // generic whole-menu implementation and InventoryMenu refines the
        // insert priority.
        virtual ContainerClickResult HandleCreativePickup(const ItemStack& source, uint8_t button);
        virtual ContainerClickResult HandleCreativeQuickMove(const ItemStack& source);
        virtual ContainerClickResult HandleCreativeDestroyAll();
        virtual ContainerClickResult HandleCreativeFillSlot(int slotIndex, const ItemStack& source);

        static void MarkChanged(ContainerClickResult& r, int slot);

    private:
        ContainerClickResult HandlePickup    (int slotIndex, uint8_t button);
        ContainerClickResult HandleQuickMove (int slotIndex);
        ContainerClickResult HandleSwap      (int slotIndex, uint8_t button);
        ContainerClickResult HandleClone     (int slotIndex);
        ContainerClickResult HandleThrow     (int slotIndex, uint8_t button);
        ContainerClickResult HandleQuickCraft(int slotIndex, uint8_t button);
        ContainerClickResult HandlePickupAll (int slotIndex);

        // Drop the cursor (slotIndex == OUTSIDE). MC convention: button 0 drops
        // the whole stack, button 1 drops one. Shared by the PICKUP and THROW
        // entry points so the two cannot disagree — they once carried opposite
        // conventions, with the call sites flipping the bit to compensate.
        void DropCarriedOutside(uint8_t button, ContainerClickResult& result);

        void ResetQuickCraft();

        std::vector<std::unique_ptr<Slot>> m_slots;
        Inventory* m_playerInventory = nullptr;
        ItemStack  m_carried{};

        // MC quickcraftStatus / quickcraftType / quickcraftSlots.
        uint8_t          m_quickcraftStatus = 0;
        uint8_t          m_quickcraftType   = 0;
        std::vector<int> m_quickcraftSlots;   // menu indices
    };

} // namespace Game
