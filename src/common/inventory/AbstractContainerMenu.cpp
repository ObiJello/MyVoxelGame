// File: src/common/inventory/AbstractContainerMenu.cpp
// Mirrors net.minecraft.world.inventory.AbstractContainerMenu.doClick().
#include "AbstractContainerMenu.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/Item.hpp"
#include <algorithm>

namespace Game {

    // MC: AbstractContainerMenu.getQuickcraftHeader/Type (lines 749, 753)
    static inline int QuickcraftHeader(int mask) { return (mask >> 2) & 3; }
    static inline int QuickcraftType  (int mask) { return mask & 3; }

    // Mirrors AbstractContainerMenu.canItemQuickReplace — may `carried` be
    // drag-distributed into a slot currently holding `slotStack`? Empty slots
    // always qualify; an occupied slot must hold the same item AND the same
    // components, and must have room.
    static bool CanItemQuickReplace(const ItemStack& slotStack,
                                    const ItemStack& carried,
                                    bool ignoreSize) {
        if (slotStack.IsEmpty()) return true;
        if (!IsSameItemSameComponents(carried, slotStack)) return false;
        const int maxStack = ItemRegistry::Get(carried.itemId).maxStackSize;
        return slotStack.count + (ignoreSize ? 0 : carried.count) <= maxStack;
    }

    void AbstractContainerMenu::MarkChanged(ContainerClickResult& r, int slot) {
        const auto v = static_cast<uint8_t>(slot);
        if (std::find(r.changedSlots.begin(), r.changedSlots.end(), v) == r.changedSlots.end()) {
            r.changedSlots.push_back(v);
        }
    }

    Slot& AbstractContainerMenu::AddSlot(std::unique_ptr<Slot> slot) {
        slot->index = static_cast<int>(m_slots.size());
        m_slots.push_back(std::move(slot));
        return *m_slots.back();
    }

    void AbstractContainerMenu::ResetQuickCraft() {
        m_quickcraftStatus = 0;
        m_quickcraftType   = 0;
        m_quickcraftSlots.clear();
    }

    // ─── moveItemStackTo ───────────────────────────────────────────────────
    // Verbatim port of AbstractContainerMenu.moveItemStackTo: a merge pass over
    // the whole range, then a single empty slot. MC calls this in a loop from
    // doClick's QUICK_MOVE branch, which is what lets an oversized source spill
    // across several empty slots.
    bool AbstractContainerMenu::MoveItemStackTo(ItemStack& stack, int begin, int end,
                                                bool backwards,
                                                ContainerClickResult& result) {
        bool anythingChanged = false;

        // Pass 1 — merge into existing stacks of the same item AND components.
        const int itemMax = ItemRegistry::Get(stack.itemId).maxStackSize;
        if (itemMax > 1) {
            int i = backwards ? end - 1 : begin;
            while (!stack.IsEmpty() && (backwards ? i >= begin : i < end)) {
                Slot& slot = GetSlot(i);
                ItemStack& target = slot.GetItemMut();
                if (!target.IsEmpty() && IsSameItemSameComponents(stack, target)) {
                    const int maxStackSize = slot.GetMaxStackSize(target);
                    const int total = target.count + stack.count;
                    if (total <= maxStackSize) {
                        stack.Clear();
                        target.count = total;
                        slot.SetChanged();
                        MarkChanged(result, i);
                        anythingChanged = true;
                    } else if (target.count < maxStackSize) {
                        stack.count -= (maxStackSize - target.count);
                        target.count = maxStackSize;
                        slot.SetChanged();
                        MarkChanged(result, i);
                        anythingChanged = true;
                    }
                }
                i += backwards ? -1 : 1;
            }
        }

        // Pass 2 — first empty slot that accepts it.
        if (!stack.IsEmpty()) {
            int i = backwards ? end - 1 : begin;
            while (backwards ? i >= begin : i < end) {
                Slot& slot = GetSlot(i);
                if (slot.GetItem().IsEmpty() && slot.MayPlace(stack)) {
                    const int maxStackSize = slot.GetMaxStackSize(stack);
                    ItemStack placed = stack;          // components ride along
                    placed.count = std::min(stack.count, maxStackSize);
                    slot.SetByPlayer(placed);
                    stack.count -= placed.count;
                    if (stack.count <= 0) stack.Clear();
                    MarkChanged(result, i);
                    anythingChanged = true;
                    break;
                }
                i += backwards ? -1 : 1;
            }
        }

        return anythingChanged;
    }

    // ─── Item click-behaviour overrides (bundle) ──────────────────────────
    // Mirrors AbstractContainerMenu.tryItemClickBehaviourOverride: the carried
    // stack's overrideStackedOnOther gets first refusal, then the slot stack's
    // overrideOtherStackedOnMe. Returning true consumes the click before the
    // normal pickup logic.
    static bool TryItemClickBehaviourOverride(AbstractContainerMenu& menu, int slotIndex,
                                              uint8_t button, ContainerClickResult& result) {
        const ClickAction action = (button == 0) ? ClickAction::PRIMARY
                                                 : ClickAction::SECONDARY;
        ItemStack& carried = menu.getCarried();
        ItemStack& slot    = menu.GetSlot(slotIndex).GetItemMut();

        if (!carried.IsEmpty()) {
            const Item& carriedItem = ItemRegistry::Get(carried.itemId);
            if (carriedItem.overrideStackedOnOther
                && carriedItem.overrideStackedOnOther(carried, menu, slotIndex,
                                                      action, result)) {
                return true;
            }
        }
        if (!slot.IsEmpty()) {
            const Item& slotItem = ItemRegistry::Get(slot.itemId);
            if (slotItem.overrideOtherStackedOnMe
                && slotItem.overrideOtherStackedOnMe(slot, carried, menu, slotIndex,
                                                     action, result)) {
                return true;
            }
        }
        return false;
    }

    // ─── OUTSIDE drop (shared by PICKUP and THROW) ─────────────────────────
    // MC's doClick handles SLOT_CLICKED_OUTSIDE once, in the PICKUP branch:
    //   PRIMARY   (button 0) → drop(carried, true); carried = EMPTY
    //   SECONDARY (button 1) → drop(carried.split(1), true)
    void AbstractContainerMenu::DropCarriedOutside(uint8_t button,
                                                   ContainerClickResult& result) {
        if (m_carried.IsEmpty()) return;
        if (button == 0) {
            result.droppedItem = m_carried;    // component-preserving copy
            m_carried.Clear();
        } else {
            result.droppedItem = m_carried;
            result.droppedItem.count = 1;
            m_carried.count--;
            if (m_carried.count <= 0) m_carried.Clear();
        }
        result.carriedChanged = true;
    }

    // ─── PICKUP ────────────────────────────────────────────────────────────
    ContainerClickResult AbstractContainerMenu::HandlePickup(int slotIndex, uint8_t button) {
        ContainerClickResult result;

        if (slotIndex == Network::InventorySlotSentinel::OUTSIDE) {
            DropCarriedOutside(button, result);
            return result;
        }
        if (!IsValidSlotIndex(slotIndex)) return result;

        // Bundle click-to-insert/extract consumes the click first.
        if (TryItemClickBehaviourOverride(*this, slotIndex, button, result)) {
            return result;
        }

        Slot& s = GetSlot(slotIndex);
        ItemStack& slot = s.GetItemMut();

        if (slot.IsEmpty()) {
            if (m_carried.IsEmpty()) return result;
            if (!s.MayPlace(m_carried)) return result;
            // Clamp to the SLOT's capacity, not the item's — MC Slot.safeInsert
            // uses getMaxStackSize(stack) = min(slot limit, item limit).
            const int maxStack = s.GetMaxStackSize(m_carried);
            int amount = (button == 0) ? m_carried.count : 1;
            amount = std::min(amount, maxStack);
            ItemStack placed = m_carried;      // components ride along
            placed.count = amount;
            s.SetByPlayer(placed);
            m_carried.count -= amount;
            if (m_carried.count <= 0) m_carried.Clear();
            result.carriedChanged = true;
            MarkChanged(result, slotIndex);
            return result;
        }

        if (m_carried.IsEmpty()) {
            if (!s.MayPickup()) return result;
            // Left = whole stack, right = ceil(count/2)
            const int amount = (button == 0) ? slot.count : (slot.count + 1) / 2;
            m_carried = s.SafeTake(amount, amount);
            result.carriedChanged = true;
            MarkChanged(result, slotIndex);
            return result;
        }

        // Both non-empty. MC branches on isSameItemSameComponents, NOT on item
        // id: two stacks of the same item with different components (a
        // Sharpness book vs a Protection book) must SWAP, not merge.
        if (IsSameItemSameComponents(slot, m_carried)) {
            if (!s.MayPlace(m_carried)) return result;
            const int maxStack = s.GetMaxStackSize(m_carried);
            const int amount = (button == 0) ? m_carried.count : 1;
            const int moved = std::min(amount, maxStack - slot.count);
            if (moved > 0) {
                slot.count += moved;
                m_carried.count -= moved;
                if (m_carried.count <= 0) m_carried.Clear();
                s.SetChanged();
                result.carriedChanged = true;
                MarkChanged(result, slotIndex);
            }
        } else {
            if (!s.MayPlace(m_carried)) return result;
            if (!s.MayPickup()) return result;
            // MC guards the swap with `carried.getCount() <=
            // slot.getMaxStackSize(carried)` and otherwise does nothing —
            // swapping a stack larger than the slot can hold would leave an
            // over-full slot (e.g. 64 carved pumpkins into the helmet slot).
            if (m_carried.count > s.GetMaxStackSize(m_carried)) return result;
            std::swap(slot, m_carried);
            s.SetChanged();
            result.carriedChanged = true;
            MarkChanged(result, slotIndex);
        }

        return result;
    }

    // ─── QUICK_MOVE (shift+click) ──────────────────────────────────────────
    // MC loops quickMoveStack until it stops moving anything, which is what
    // lets one shift-click spill a stack across several empty slots.
    ContainerClickResult AbstractContainerMenu::HandleQuickMove(int slotIndex) {
        ContainerClickResult result;
        if (!IsValidSlotIndex(slotIndex)) return result;
        if (!GetSlot(slotIndex).MayPickup()) return result;

        for (;;) {
            const ItemStack before = GetSlot(slotIndex).GetItem();
            if (before.IsEmpty()) break;
            QuickMoveStack(slotIndex, result);
            const ItemStack& after = GetSlot(slotIndex).GetItem();
            // Stop as soon as an iteration achieves nothing, otherwise a slot
            // whose contents cannot move anywhere would spin forever.
            if (after.count == before.count && IsSameItemSameComponents(after, before)) break;
        }
        return result;
    }

    // ─── SWAP (number key / F) ────────────────────────────────────────────
    // MC lines 483-519. `button` is a PLAYER-INVENTORY index, not a menu index:
    // 0..8 select a hotbar slot, and 40 is MC's offhand (the F key).
    ContainerClickResult AbstractContainerMenu::HandleSwap(int slotIndex, uint8_t button) {
        ContainerClickResult result;
        if (!IsValidSlotIndex(slotIndex)) return result;

        // MC: `buttonNum >= 0 && buttonNum < 9 || buttonNum == 40`. 40 is the
        // offhand in MC's Inventory index space; ours lives at OFFHAND_BEGIN.
        constexpr uint8_t MC_OFFHAND_BUTTON = 40;
        int sourceIdx;
        if (button < Inventory::HOTBAR_SIZE) {
            sourceIdx = Inventory::HotbarToIndex(button);
        } else if (button == MC_OFFHAND_BUTTON) {
            sourceIdx = Inventory::OFFHAND_BEGIN;
        } else {
            return result;
        }
        if (sourceIdx == slotIndex) return result;
        if (!IsValidSlotIndex(sourceIdx)) return result;

        Slot& targetSlot = GetSlot(slotIndex);
        Slot& sourceSlot = GetSlot(sourceIdx);
        ItemStack& source = sourceSlot.GetItemMut();
        ItemStack& target = targetSlot.GetItemMut();

        if (source.IsEmpty() && target.IsEmpty()) return result;

        if (source.IsEmpty()) {
            if (!targetSlot.MayPickup()) return result;
            source = target;
            target.Clear();
        } else if (target.IsEmpty()) {
            if (!targetSlot.MayPlace(source)) return result;
            const int maxStack = targetSlot.GetMaxStackSize(source);
            if (source.count > maxStack) {
                // Oversized source splits — the target takes what it can hold
                // and the remainder stays put (MC: target.setByPlayer(
                // source.split(maxStackSize))).
                target = source;
                target.count = maxStack;
                source.count -= maxStack;
            } else {
                target = source;
                source.Clear();
            }
        } else {
            if (!targetSlot.MayPickup() || !targetSlot.MayPlace(source)) return result;
            const int maxStack = targetSlot.GetMaxStackSize(source);
            if (source.count > maxStack) {
                // MC drops the displaced target stack on the floor when it
                // cannot be re-added; with no item-entity system, refusing the
                // swap is the only non-destructive option.
                return result;
            }
            std::swap(source, target);
        }

        sourceSlot.SetChanged();
        targetSlot.SetChanged();
        MarkChanged(result, slotIndex);
        MarkChanged(result, sourceIdx);
        return result;
    }

    // ─── CLONE (middle click, creative) ───────────────────────────────────
    ContainerClickResult AbstractContainerMenu::HandleClone(int slotIndex) {
        ContainerClickResult result;
        if (!IsValidSlotIndex(slotIndex)) return result;
        if (!m_carried.IsEmpty()) return result;

        const ItemStack& src = GetSlot(slotIndex).GetItem();
        if (src.IsEmpty()) return result;

        // Component-preserving copy — a cloned enchanted book keeps its
        // enchantments (MC clones the full stack).
        m_carried = src;
        m_carried.count = ItemRegistry::Get(src.itemId).maxStackSize;
        result.carriedChanged = true;
        return result;
    }

    // ─── THROW (Q key, drop) ───────────────────────────────────────────────
    // MC lines 526-546. Drop 1 (button 0) or the whole stack (button 1).
    ContainerClickResult AbstractContainerMenu::HandleThrow(int slotIndex, uint8_t button) {
        ContainerClickResult result;
        if (slotIndex == Network::InventorySlotSentinel::OUTSIDE) {
            DropCarriedOutside(button, result);
            return result;
        }
        if (!IsValidSlotIndex(slotIndex)) return result;
        if (!m_carried.IsEmpty()) return result;   // MC requires an empty cursor

        Slot& s = GetSlot(slotIndex);
        if (s.GetItem().IsEmpty()) return result;

        const int amount = (button == 0) ? 1 : s.GetItem().count;
        result.droppedItem = s.SafeTake(amount, amount);
        if (result.droppedItem.IsEmpty()) return result;
        MarkChanged(result, slotIndex);
        return result;
    }

    // ─── QUICK_CRAFT (drag-distribute) ────────────────────────────────────
    // MC lines 358-414. Three phases via button = getQuickcraftMask(header, type).
    ContainerClickResult AbstractContainerMenu::HandleQuickCraft(int slotIndex, uint8_t button) {
        ContainerClickResult result;
        const int header = QuickcraftHeader(button);
        const int type   = QuickcraftType(button);

        // Phase transition validation (MC line 361). 1→2 is the legitimate
        // end-of-drag transition; any other deviation resets the drag.
        const int expected = m_quickcraftStatus;
        m_quickcraftStatus = (uint8_t)header;

        if ((expected != 1 || header != 2) && expected != header) { ResetQuickCraft(); return result; }
        if (m_carried.IsEmpty()) { ResetQuickCraft(); return result; }

        if (header == 0) {
            m_quickcraftType   = (uint8_t)type;
            m_quickcraftStatus = 1;
            m_quickcraftSlots.clear();
            return result;
        }

        if (header == 1) {
            if (!IsValidSlotIndex(slotIndex)) return result;
            Slot& s = GetSlot(slotIndex);
            if (!s.MayPlace(m_carried)) return result;
            // Slot must be empty, or hold the same item AND components
            // (MC: canItemQuickReplace(slot, carried, true)).
            if (!CanItemQuickReplace(s.GetItem(), m_carried, true)) return result;
            if (std::find(m_quickcraftSlots.begin(), m_quickcraftSlots.end(), slotIndex)
                != m_quickcraftSlots.end()) return result;
            // MC: type == 2 || carried.getCount() > quickcraftSlots.size()
            if (m_quickcraftType != 2
                && (int)m_carried.count <= (int)m_quickcraftSlots.size()) return result;
            m_quickcraftSlots.push_back(slotIndex);
            return result;
        }

        if (header == 2) {
            if (m_quickcraftSlots.empty()) { ResetQuickCraft(); return result; }

            // Single slot → fall back to PICKUP (MC line 381).
            if (m_quickcraftSlots.size() == 1) {
                const int singleSlot = m_quickcraftSlots.front();
                const uint8_t pickupBtn = m_quickcraftType;
                ResetQuickCraft();
                return HandlePickup(singleSlot, pickupBtn);
            }

            const int dragType     = m_quickcraftType;
            const int totalCarried = m_carried.count;
            const int itemMaxStack = ItemRegistry::Get(m_carried.itemId).maxStackSize;
            const int slotCount    = (int)m_quickcraftSlots.size();
            int remaining = totalCarried;

            // Per-slot distribution — MC getQuickCraftPlaceCount:
            //   type 0 (left,  split):    floor(carried / slotCount)
            //   type 1 (right, one each): 1
            //   type 2 (middle, clone):   itemStack.getMaxStackSize()
            // Clone is a FULL STACK per slot, not the carried count: dragging a
            // single item across slots in creative fills each to 64.
            int perSlot;
            if (dragType == 0)      perSlot = totalCarried / slotCount;
            else if (dragType == 1) perSlot = 1;
            else                    perSlot = itemMaxStack;

            for (int idx : m_quickcraftSlots) {
                if (remaining <= 0 && dragType != 2) break;
                Slot& s = GetSlot(idx);
                ItemStack& target = s.GetItemMut();
                // MC re-validates every slot at commit time — the drag set was
                // built click-by-click and the container may have moved on
                // since. Mirrors the end-phase guard in doClick.
                if (!CanItemQuickReplace(target, m_carried, true)) continue;
                if (!s.MayPlace(m_carried)) continue;
                if (dragType != 2 && totalCarried < slotCount) continue;

                const int slotMax  = s.GetMaxStackSize(m_carried);
                const int existing = target.IsEmpty() ? 0 : target.count;
                const int toPlace  = std::min(perSlot + existing, slotMax);
                const int delta    = toPlace - existing;
                if (delta <= 0) continue;

                ItemStack placed = m_carried;   // components ride along
                placed.count = toPlace;
                s.SetByPlayer(placed);
                if (dragType != 2) remaining -= delta;
                MarkChanged(result, idx);
            }

            // Clone (creative) leaves the cursor untouched — you are dragging
            // from an infinite source.
            if (dragType != 2) {
                m_carried.count = remaining;
                if (m_carried.count <= 0) m_carried.Clear();
            }
            result.carriedChanged = true;
            ResetQuickCraft();
            return result;
        }

        ResetQuickCraft();
        return result;
    }

    // ─── PICKUP_ALL (double-click) ────────────────────────────────────────
    ContainerClickResult AbstractContainerMenu::HandlePickupAll(int slotIndex) {
        ContainerClickResult result;
        if (m_carried.IsEmpty()) return result;

        // MC precondition: `!carried.isEmpty() && (!slot.hasItem() ||
        // !slot.mayPickup(player))`. The canonical double-click is (1) pick the
        // stack up, leaving the slot EMPTY, then (2) click the now-empty slot to
        // vacuum. Collecting while the clicked slot is still occupied is not a
        // thing MC does, and skipping this check turned any second click into an
        // inventory-wide vacuum.
        if (IsValidSlotIndex(slotIndex)) {
            Slot& clicked = GetSlot(slotIndex);
            if (clicked.HasItem() && clicked.MayPickup()) return result;
        }

        const int maxStack = ItemRegistry::Get(m_carried.itemId).maxStackSize;

        // Two-pass collect: partial stacks first (so full stacks survive), then
        // full ones. MC walks the whole menu; we skip slots that refuse to give
        // their contents up.
        for (int pass = 0; pass < 2 && m_carried.count < maxStack; ++pass) {
            for (int i = 0; i < SlotCount() && m_carried.count < maxStack; ++i) {
                Slot& s = GetSlot(i);
                if (!s.MayPickup()) continue;
                const ItemStack& stack = s.GetItem();
                if (stack.IsEmpty() || !IsSameItemSameComponents(stack, m_carried)) continue;
                if (pass == 0 && stack.count >= maxStack) continue;   // partials first
                const ItemStack taken = s.SafeTake(stack.count, maxStack - m_carried.count);
                if (taken.IsEmpty()) continue;
                m_carried.count += taken.count;
                MarkChanged(result, i);
            }
        }
        result.carriedChanged = true;
        return result;
    }

    // ─── Creative paths ────────────────────────────────────────────────────
    // The search grid is an infinite source. Left-click fills the cursor with a
    // full stack; right-click takes one (or increments an identical cursor).
    ContainerClickResult AbstractContainerMenu::HandleCreativePickup(const ItemStack& source,
                                                                     uint8_t button) {
        ContainerClickResult result;
        if (source.itemId == Items::Air) return result;
        const int maxStack = ItemRegistry::Get(source.itemId).maxStackSize;

        // Holding something else? Clicking the grid discards it and does NOT
        // pick the new item up — two clicks to swap. This makes the search grid
        // the de-facto delete gesture for whatever you grabbed.
        //
        // "Something else" means a different item OR different components: the
        // grid lists one cell per enchanted-book variant, all sharing the
        // enchanted_book item id. An id-only test let a right-click on
        // Protection increment a held stack of Sharpness.
        if (!m_carried.IsEmpty() && !IsSameItemSameComponents(m_carried, source)) {
            m_carried.Clear();
            result.carriedChanged = true;
            return result;
        }

        if (button == 0) {
            m_carried = source;             // identity copy — components ride along
            m_carried.count = maxStack;
        } else if (m_carried.IsEmpty()) {
            m_carried = source;
            m_carried.count = 1;
        } else if (m_carried.count < maxStack) {
            m_carried.count++;
        }
        result.carriedChanged = true;
        return result;
    }

    // Shift-click on the search grid: drop a full stack into the menu.
    ContainerClickResult AbstractContainerMenu::HandleCreativeQuickMove(const ItemStack& source) {
        ContainerClickResult result;
        if (source.itemId == Items::Air) return result;
        ItemStack stack = source;
        stack.count = ItemRegistry::Get(source.itemId).maxStackSize;
        MoveItemStackTo(stack, 0, SlotCount(), false, result);
        return result;
    }

    // Shift-click on the destroy_item slot — mirrors
    // CreativeModeInventoryScreen.slotClicked() lines 189-193.
    ContainerClickResult AbstractContainerMenu::HandleCreativeDestroyAll() {
        ContainerClickResult result;
        // Mark EVERY slot changed, not just the ones the server thought were
        // occupied: the client may hold predicted pickups the server never
        // recorded, and those would otherwise leave ghost stacks with no delta
        // to correct them.
        for (int i = 0; i < SlotCount(); ++i) {
            GetSlot(i).Set(ItemStack{});
            MarkChanged(result, i);
        }
        m_carried.Clear();
        result.carriedChanged = true;
        return result;
    }

    // Pick-block (P key), routed through the server so the authoritative
    // inventory reflects it. Mirrors ServerboundSetCreativeModeSlotPacket.
    ContainerClickResult AbstractContainerMenu::HandleCreativeFillSlot(int slotIndex,
                                                                       const ItemStack& source) {
        ContainerClickResult result;
        if (!IsValidSlotIndex(slotIndex)) return result;
        Slot& s = GetSlot(slotIndex);

        if (source.itemId == Items::Air) {
            // Treat Air as "clear the slot" — matches pick-block on an air block.
            if (s.HasItem()) {
                s.Set(ItemStack{});
                MarkChanged(result, slotIndex);
            }
            return result;
        }
        // Same placement filter as a regular click — pick-block onto an armor
        // slot must not bypass the EQUIPPABLE check, and must deposit 1 rather
        // than a full item-sized stack.
        if (!s.MayPlace(source)) return result;
        ItemStack placed = source;          // identity copy — components ride along
        placed.count = s.GetMaxStackSize(source);
        s.SetByPlayer(placed);
        MarkChanged(result, slotIndex);
        return result;
    }

    // ─── DISPATCH ──────────────────────────────────────────────────────────
    ContainerClickResult AbstractContainerMenu::DoClick(
            const Network::InventoryClickC2SPacket& click) {
        const auto action = static_cast<Network::ContainerInput>(click.action);
        const int  slot   = click.slotIndex;

        // Any non-QUICK_CRAFT action mid-drag aborts the drag (MC line 415).
        if (action != Network::ContainerInput::QUICK_CRAFT && m_quickcraftStatus != 0) {
            ResetQuickCraft();
        }

        // Search-tab creative-grid clicks. Prefer the full creativeStack (it
        // carries per-stack components, e.g. enchanted-book variants); fall back
        // to the bare id for old-format packets.
        const ItemStack creativeSource =
            !click.creativeStack.IsEmpty()
                ? click.creativeStack
                : ItemStack{static_cast<ItemID>(click.creativeItemId), 1};

        if (slot == Network::InventorySlotSentinel::CREATIVE_GRID) {
            switch (action) {
                case Network::ContainerInput::PICKUP:
                    return HandleCreativePickup(creativeSource, click.button);
                case Network::ContainerInput::QUICK_MOVE:
                    return HandleCreativeQuickMove(creativeSource);
                case Network::ContainerInput::CLONE:
                    // Middle click on a creative source == left-click pickup.
                    return HandleCreativePickup(creativeSource, 0);
                default:
                    return {};
            }
        }

        switch (action) {
            case Network::ContainerInput::PICKUP:      return HandlePickup    (slot, click.button);
            case Network::ContainerInput::QUICK_MOVE:  return HandleQuickMove (slot);
            case Network::ContainerInput::SWAP:        return HandleSwap      (slot, click.button);
            case Network::ContainerInput::CLONE:       return HandleClone     (slot);
            case Network::ContainerInput::THROW:       return HandleThrow     (slot, click.button);
            case Network::ContainerInput::QUICK_CRAFT: return HandleQuickCraft(slot, click.button);
            case Network::ContainerInput::PICKUP_ALL:  return HandlePickupAll (slot);
            case Network::ContainerInput::CREATIVE_DESTROY_ALL:
                return HandleCreativeDestroyAll();
            case Network::ContainerInput::CREATIVE_FILL_SLOT:
                return HandleCreativeFillSlot(slot, creativeSource);
            default:
                Log::Warning("[AbstractContainerMenu] Unknown action %u", (unsigned)click.action);
                return {};
        }
    }

} // namespace Game
