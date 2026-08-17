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

#include "ContainerData.hpp"
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
        // Populated for THROW and for a click outside the window: the stack the
        // player deliberately threw away. The session spawns it as a world
        // entity.
        ItemStack            droppedItem{};
        // Stacks a closing menu could not hand back because the inventory was
        // full (MC clearContainer → Containers.drop). These were never the
        // menu's to keep, so they go into the world rather than being deleted.
        // Separate from droppedItem because a single close can strand several.
        std::vector<ItemStack> extraDrops;
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

        // MC AbstractContainerMenu.stateId — the revision of the last full sync
        // the CLIENT received. Stamped onto every outgoing click so the server
        // can spot one predicted against stale state. Lives on the menu rather
        // than on a screen because both inventory screens share this menu and
        // must agree about which snapshot they are clicking against. The server
        // keeps its own authoritative counter in PlayerSession.
        uint32_t stateId = 0;

        // ── Click dispatch ────────────────────────────────────────────────
        // Apply one click. Same call on both sides: the client runs it to
        // predict, the server to decide.
        ContainerClickResult DoClick(const Network::InventoryClickC2SPacket& click);

        // MC AbstractContainerMenu.quickMoveStack — where does a shift-click on
        // `slotIndex` send its contents? Menu-specific, hence abstract.
        virtual void QuickMoveStack(int slotIndex, ContainerClickResult& result) = 0;

        // MC AbstractContainerMenu.slotsChanged — this menu's contents moved;
        // recompute anything derived from them. The crafting menus re-run the
        // recipe lookup here and rewrite their result square.
        //
        // Called once at the end of every DoClick, and again inside the
        // quick-move repeat loop so a shift-click on a crafting result can
        // craft over and over exactly as MC's does. Running it on BOTH sides
        // means the result square updates the instant you place an ingredient,
        // with no round trip.
        virtual void SlotsChanged(ContainerClickResult& result) { (void)result; }

        // MC AbstractContainerMenu.removed — the menu is closing. A menu that
        // owns storage the player cannot otherwise reach (a crafting grid)
        // hands it back here so nothing is stranded.
        virtual void Removed(ContainerClickResult& result) { (void)result; }

        // Menu index of a player-inventory slot, or -1 when this menu does not
        // show it. The identity for InventoryMenu; offset for menus that put
        // their own container first. Used by anything holding an inventory
        // index that needs to speak the menu's index space — notably
        // PlayerSession's remote-slot bookkeeping.
        virtual int MenuIndexForInventorySlot(int inventoryIndex) const {
            return IsValidSlotIndex(inventoryIndex) ? inventoryIndex : -1;
        }

        // ── Data slots (MC addDataSlots / ContainerData) ──────────────────
        // State the client must see that isn't an item: furnace burn + cook
        // timers, brewing progress, enchantment offers. Null for menus with
        // none, which is most of them. The server diffs these every tick
        // alongside the slots and sends ContainerSetDataS2C per changed index;
        // the client's copy is always a SimpleContainerData it just receives
        // into. See ContainerData.hpp.
        int  DataCount() const { return m_data ? m_data->Count() : 0; }
        int  GetData(int index) const { return m_data ? m_data->Get(index) : 0; }
        void SetData(int index, int value) { if (m_data) m_data->Set(index, value); }

    protected:
        explicit AbstractContainerMenu(Inventory* playerInventory)
            : m_playerInventory(playerInventory) {}

        // Append a slot; assigns its menu index. Mirrors MC addSlot.
        Slot& AddSlot(std::unique_ptr<Slot> slot);

        // Mirrors MC addDataSlots(ContainerData). Ownership stays with the
        // caller when it's a block entity's live view (DelegatingContainerData
        // over the BE's own counters); menus with no BE behind them hand over a
        // SimpleContainerData they own via SetOwnedData.
        void SetData(ContainerData* data) { m_data = data; }
        void SetOwnedData(std::unique_ptr<ContainerData> data) {
            m_ownedData = std::move(data);
            m_data = m_ownedData.get();
        }

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

        // Borrowed (block-entity-backed) or owned (SimpleContainerData) — see
        // SetData / SetOwnedData. m_data always points at whichever is live.
        ContainerData*                 m_data = nullptr;
        std::unique_ptr<ContainerData> m_ownedData;

        // MC quickcraftStatus / quickcraftType / quickcraftSlots.
        uint8_t          m_quickcraftStatus = 0;
        uint8_t          m_quickcraftType   = 0;
        std::vector<int> m_quickcraftSlots;   // menu indices
    };

} // namespace Game
