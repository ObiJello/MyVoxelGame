// File: src/server/inventory/InventoryClickHandler.cpp
// Mirrors net.minecraft.world.inventory.AbstractContainerMenu.doClick().
#include "InventoryClickHandler.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/data/DataComponents.hpp"
#include <algorithm>

namespace Server {

    using Game::Inventory;
    using Game::InventorySlot;
    using Game::ItemID;
    using Game::ItemRegistry;

    // MC: AbstractContainerMenu.getQuickcraftHeader/Type/Mask (lines 749, 753, 757)
    static inline int QuickcraftHeader(int mask) { return (mask >> 2) & 3; }
    static inline int QuickcraftType  (int mask) { return mask & 3; }

    void InventoryClickHandler::MarkChanged(InventoryClickResult& r, uint8_t slot) {
        if (std::find(r.changedSlots.begin(), r.changedSlots.end(), slot) == r.changedSlots.end()) {
            r.changedSlots.push_back(slot);
        }
    }

    bool InventoryClickHandler::MayPlaceInSlot(int16_t slotIndex,
                                               const Game::InventorySlot& stack) {
        // Shared filter in common/ — the client's InventoryScreen prediction
        // uses the same function so refused clicks never leave ghost items.
        return Game::MayPlaceInSlot(slotIndex, stack);
    }

    int InventoryClickHandler::MergeInto(InventorySlot& dst, InventorySlot& src) {
        if (src.IsEmpty() || dst.itemId != src.itemId) return 0;
        const int maxStack = ItemRegistry::Get(dst.itemId).maxStackSize;
        int free = maxStack - dst.count;
        if (free <= 0) return 0;
        int moved = std::min(free, src.count);
        dst.count += moved;
        src.count -= moved;
        if (src.count <= 0) src.Clear();
        return moved;
    }

    bool InventoryClickHandler::MoveStackToRegion(Inventory& inv, int sourceIndex,
                                                  int rangeBegin, int rangeEnd,
                                                  InventoryClickResult& result) {
        InventorySlot& source = inv.MutableSlot(sourceIndex);
        if (source.IsEmpty()) return false;
        bool moved = false;

        // Pass 1: merge into existing same-item stacks
        const int srcMaxStack = ItemRegistry::Get(source.itemId).maxStackSize;
        for (int i = rangeBegin; i < rangeEnd && !source.IsEmpty(); ++i) {
            if (i == sourceIndex) continue;
            InventorySlot& dst = inv.MutableSlot(i);
            if (dst.itemId == source.itemId && !dst.IsEmpty() && dst.count < srcMaxStack) {
                int n = MergeInto(dst, source);
                if (n > 0) { moved = true; MarkChanged(result, (uint8_t)i); }
            }
        }

        // Pass 2: drop into empty slots
        for (int i = rangeBegin; i < rangeEnd && !source.IsEmpty(); ++i) {
            if (i == sourceIndex) continue;
            InventorySlot& dst = inv.MutableSlot(i);
            if (dst.IsEmpty()) {
                dst = source;
                source.Clear();
                moved = true;
                MarkChanged(result, (uint8_t)i);
                break;
            }
        }

        if (moved) MarkChanged(result, (uint8_t)sourceIndex);
        return moved;
    }

    // ─── Item click-behaviour overrides (bundle) ──────────────────────────
    // Mirrors AbstractContainerMenu.tryItemClickBehaviourOverride: the
    // carried stack's overrideStackedOnOther gets first refusal, then the
    // slot stack's overrideOtherStackedOnMe. Returning true consumes the
    // click before the normal pickup logic.
    static bool TryItemClickBehaviourOverride(ServerPlayer& player, int16_t slotIndex,
                                              uint8_t button, InventoryClickResult& result) {
        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return false;
        const Game::ClickAction action = (button == 0) ? Game::ClickAction::PRIMARY
                                                       : Game::ClickAction::SECONDARY;
        Inventory& inv = player.getInventory();
        InventorySlot& carried = player.getCarried();
        InventorySlot& slot    = inv.MutableSlot(slotIndex);

        if (!carried.IsEmpty()) {
            const Game::Item& carriedItem = ItemRegistry::Get(carried.itemId);
            if (carriedItem.overrideStackedOnOther
                && carriedItem.overrideStackedOnOther(carried, inv, slotIndex,
                                                      action, player, result)) {
                return true;
            }
        }
        if (!slot.IsEmpty()) {
            const Game::Item& slotItem = ItemRegistry::Get(slot.itemId);
            if (slotItem.overrideOtherStackedOnMe
                && slotItem.overrideOtherStackedOnMe(slot, carried, inv, slotIndex,
                                                     action, player, result)) {
                return true;
            }
        }
        return false;
    }

    // ─── PICKUP ────────────────────────────────────────────────────────────
    // MC: AbstractContainerMenu.java lines 417-482
    InventoryClickResult InventoryClickHandler::HandlePickup(ServerPlayer& player,
                                                             int16_t slotIndex, uint8_t button) {
        InventoryClickResult result;
        Inventory& inv = player.getInventory();
        InventorySlot& carried = player.getCarried();

        // Click outside the panel: drop carried.
        if (slotIndex == Network::InventorySlotSentinel::OUTSIDE) {
            if (!carried.IsEmpty()) {
                if (button == 0) {
                    // Left-click outside drops the entire stack
                    result.droppedItem = carried;
                    carried.Clear();
                } else {
                    // Right-click outside drops 1 (component-preserving copy)
                    result.droppedItem = carried;
                    result.droppedItem.count = 1;
                    carried.count--;
                    if (carried.count <= 0) carried.Clear();
                }
                result.carriedChanged = true;
            }
            return result;
        }

        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;

        // Bundle click-to-insert/extract — consumes the click before the
        // normal pickup/merge/swap (AbstractContainerMenu.doClick's
        // tryItemClickBehaviourOverride call).
        if (TryItemClickBehaviourOverride(player, slotIndex, button, result)) {
            return result;
        }

        InventorySlot& slot = inv.MutableSlot(slotIndex);

        if (slot.IsEmpty()) {
            if (carried.IsEmpty()) return result;
            // Per-slot insert filter (Slot.mayPlace): craft slots refuse,
            // armor slots accept only their matching EQUIPPABLE.
            if (!MayPlaceInSlot(slotIndex, carried)) return result;
            // Insert from cursor (component-preserving copy)
            const int maxStack = ItemRegistry::Get(carried.itemId).maxStackSize;
            int amount = (button == 0) ? carried.count : 1;
            amount = std::min(amount, maxStack);
            slot = carried;
            slot.count = amount;
            carried.count -= amount;
            if (carried.count <= 0) carried.Clear();
            result.carriedChanged = true;
            MarkChanged(result, (uint8_t)slotIndex);
            return result;
        }

        // Slot is non-empty
        if (carried.IsEmpty()) {
            // Pick up: left = full stack, right = ceil(count/2)
            // (component-preserving copy)
            int amount = (button == 0) ? slot.count : (slot.count + 1) / 2;
            carried = slot;
            carried.count = amount;
            slot.count -= amount;
            if (slot.count <= 0) slot.Clear();
            result.carriedChanged = true;
            MarkChanged(result, (uint8_t)slotIndex);
            return result;
        }

        // Both non-empty
        if (slot.itemId == carried.itemId) {
            // Same item: merge cursor → slot
            if (!MayPlaceInSlot(slotIndex, carried)) return result;
            const int maxStack = ItemRegistry::Get(slot.itemId).maxStackSize;
            int amount = (button == 0) ? carried.count : 1;
            int free = maxStack - slot.count;
            int moved = std::min(amount, free);
            slot.count += moved;
            carried.count -= moved;
            if (carried.count <= 0) carried.Clear();
            result.carriedChanged = true;
            MarkChanged(result, (uint8_t)slotIndex);
        } else {
            // Different items: swap (only when the slot accepts the cursor stack)
            if (!MayPlaceInSlot(slotIndex, carried)) return result;
            std::swap(slot, carried);
            result.carriedChanged = true;
            MarkChanged(result, (uint8_t)slotIndex);
        }

        return result;
    }

    // ─── QUICK_MOVE (shift+click) ──────────────────────────────────────────
    // MC line 428-439. Move stack between hotbar/main; from armor/craft → main+hotbar.
    InventoryClickResult InventoryClickHandler::HandleQuickMove(ServerPlayer& player,
                                                                 int16_t slotIndex, uint8_t /*button*/) {
        InventoryClickResult result;
        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;
        Inventory& inv = player.getInventory();

        // Equip priority — mirrors InventoryMenu.quickMoveStack: shift-click
        // on an EQUIPPABLE in main/hotbar tries its armor/offhand slot FIRST
        // when that slot is empty.
        if (Inventory::IsHotbarSlot(slotIndex) || Inventory::IsMainSlot(slotIndex)) {
            InventorySlot& source = inv.MutableSlot(slotIndex);
            if (!source.IsEmpty()) {
                if (auto equippable = source.get(Game::DataComponents::EQUIPPABLE)) {
                    const int target = Game::InventoryIndexFor(equippable->slot);
                    if (target >= 0 && inv.GetSlot(target).IsEmpty()) {
                        // Armor/offhand slots hold one item (armor stacksTo(1)
                        // anyway; the offhand target here mirrors the shield).
                        InventorySlot equipped = source;
                        equipped.count = 1;
                        inv.SetSlotFull(target, equipped);
                        source.count -= 1;
                        if (source.count <= 0) source.Clear();
                        MarkChanged(result, (uint8_t)target);
                        MarkChanged(result, (uint8_t)slotIndex);
                        return result;
                    }
                }
            }
        }

        if (Inventory::IsHotbarSlot(slotIndex)) {
            // Hotbar → main
            MoveStackToRegion(inv, slotIndex, Inventory::MAIN_BEGIN,
                              Inventory::MAIN_BEGIN + Inventory::MAIN_SIZE, result);
        } else if (Inventory::IsMainSlot(slotIndex)) {
            // Main → hotbar
            MoveStackToRegion(inv, slotIndex, Inventory::HOTBAR_BEGIN,
                              Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE, result);
        } else if (Inventory::IsArmorSlot(slotIndex) ||
                   Inventory::IsOffhandSlot(slotIndex) ||
                   Inventory::IsCraftGridSlot(slotIndex) ||
                   Inventory::IsCraftResultSlot(slotIndex)) {
            // Restricted source (armor/offhand/craft) → try main first, then
            // hotbar. MC's InventoryMenu.quickMoveStack routes these the same
            // way; the offhand case was previously missing so shift-clicking
            // slot 45 did nothing.
            if (!MoveStackToRegion(inv, slotIndex, Inventory::MAIN_BEGIN,
                                   Inventory::MAIN_BEGIN + Inventory::MAIN_SIZE, result)) {
                MoveStackToRegion(inv, slotIndex, Inventory::HOTBAR_BEGIN,
                                  Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE, result);
            }
        }
        return result;
    }

    // ─── SWAP (number key) ────────────────────────────────────────────────
    // MC lines 483-519. button = hotbar index 0..8.
    InventoryClickResult InventoryClickHandler::HandleSwap(ServerPlayer& player,
                                                           int16_t slotIndex, uint8_t button) {
        InventoryClickResult result;
        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;
        if (button >= Inventory::HOTBAR_SIZE) return result;

        Inventory& inv = player.getInventory();
        int hotbarIdx = Inventory::HotbarToIndex(button);
        if (hotbarIdx == slotIndex) return result;

        InventorySlot& a = inv.MutableSlot(slotIndex);
        InventorySlot& b = inv.MutableSlot(hotbarIdx);

        // Per-slot insert filter — the hotbar stack must be placeable into the
        // target slot (armor accepts only its matching EQUIPPABLE; craft refuses).
        if (!b.IsEmpty() && !MayPlaceInSlot(slotIndex, b)) return result;

        std::swap(a, b);
        MarkChanged(result, (uint8_t)slotIndex);
        MarkChanged(result, (uint8_t)hotbarIdx);
        return result;
    }

    // ─── CLONE (middle click, creative) ───────────────────────────────────
    // MC lines 520-525. Cursor must be empty; copies slot to cursor as full stack.
    InventoryClickResult InventoryClickHandler::HandleClone(ServerPlayer& player, int16_t slotIndex) {
        InventoryClickResult result;
        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;
        InventorySlot& carried = player.getCarried();
        if (!carried.IsEmpty()) return result;

        const InventorySlot& src = player.getInventory().GetSlot(slotIndex);
        if (src.IsEmpty()) return result;

        // Component-preserving copy (a cloned enchanted book keeps its
        // enchantments — MC clones the full stack).
        carried = src;
        carried.count = ItemRegistry::Get(src.itemId).maxStackSize;
        result.carriedChanged = true;
        return result;
    }

    // ─── THROW (Q key, drop) ───────────────────────────────────────────────
    // MC lines 526-546. Drop 1 (button 0) or full stack (button 1) from a slot.
    // (Outside-click drops are routed through HandlePickup; this branch only handles Q.)
    InventoryClickResult InventoryClickHandler::HandleThrow(ServerPlayer& player,
                                                            int16_t slotIndex, uint8_t button) {
        InventoryClickResult result;
        if (slotIndex == Network::InventorySlotSentinel::OUTSIDE) {
            // Drop carried (outside-click); MC handles this in PICKUP branch but we route here too.
            InventorySlot& carried = player.getCarried();
            if (!carried.IsEmpty()) {
                if (button == 0) {
                    result.droppedItem = carried;   // component-preserving copy
                    result.droppedItem.count = 1;
                    carried.count--;
                    if (carried.count <= 0) carried.Clear();
                } else {
                    result.droppedItem = carried;
                    carried.Clear();
                }
                result.carriedChanged = true;
            }
            return result;
        }
        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;
        if (!player.getCarried().IsEmpty()) return result; // MC requires empty cursor

        InventorySlot& slot = player.getInventory().MutableSlot(slotIndex);
        if (slot.IsEmpty()) return result;

        int amount = (button == 0) ? 1 : slot.count;
        amount = std::min(amount, slot.count);
        result.droppedItem = slot;   // component-preserving copy
        result.droppedItem.count = amount;
        slot.count -= amount;
        if (slot.count <= 0) slot.Clear();
        MarkChanged(result, (uint8_t)slotIndex);
        return result;
    }

    // ─── QUICK_CRAFT (drag-distribute) ────────────────────────────────────
    // MC lines 358-414. Three phases via button = getQuickcraftMask(header, type).
    InventoryClickResult InventoryClickHandler::HandleQuickCraft(ServerPlayer& player,
                                                                  int16_t slotIndex, uint8_t button) {
        InventoryClickResult result;
        const int header = QuickcraftHeader(button);
        const int type   = QuickcraftType(button);

        // Phase transition validation (MC line 361). 1→2 is the legitimate end-of-drag transition;
        // any other deviation resets quick-craft state.
        const int expected = player.m_quickcraftStatus;
        player.m_quickcraftStatus = (uint8_t)header;

        auto reset = [&]() {
            player.m_quickcraftStatus = 0;
            player.m_quickcraftType = 0;
            player.m_quickcraftSlots.clear();
        };

        if ((expected != 1 || header != 2) && expected != header) { reset(); return result; }

        InventorySlot& carried = player.getCarried();
        if (carried.IsEmpty()) { reset(); return result; }

        if (header == 0) {
            // Start phase
            player.m_quickcraftType = (uint8_t)type;
            player.m_quickcraftStatus = 1;
            player.m_quickcraftSlots.clear();
            return result;
        }
        if (header == 1) {
            // Add slot to drag set
            if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;
            if (!MayPlaceInSlot(slotIndex, carried)) return result;
            // Slot must be empty or same block as carried
            const InventorySlot& s = player.getInventory().GetSlot(slotIndex);
            if (!s.IsEmpty() && s.itemId != carried.itemId) return result;
            // No duplicate adds
            const uint8_t slotByte = (uint8_t)slotIndex;
            if (std::find(player.m_quickcraftSlots.begin(), player.m_quickcraftSlots.end(), slotByte)
                != player.m_quickcraftSlots.end()) return result;
            // For split type (0), only add if remaining cursor count >= number of slots already added
            if (player.m_quickcraftType != 2 && (int)carried.count <= (int)player.m_quickcraftSlots.size()) return result;
            player.m_quickcraftSlots.push_back(slotByte);
            return result;
        }
        if (header == 2) {
            // End phase — distribute
            if (player.m_quickcraftSlots.empty()) { reset(); return result; }

            // Special case: single slot → fall back to PICKUP (MC line 381)
            if (player.m_quickcraftSlots.size() == 1) {
                const int singleSlot = player.m_quickcraftSlots.front();
                const uint8_t pickupBtn = (uint8_t)player.m_quickcraftType;
                reset();
                Network::InventoryClickC2SPacket fallback{};
                fallback.slotIndex = (int16_t)singleSlot;
                fallback.button    = pickupBtn;
                fallback.action    = (uint8_t)Network::ContainerInput::PICKUP;
                return HandlePickup(player, fallback.slotIndex, fallback.button);
            }

            const int dragType = player.m_quickcraftType;
            const int totalCarried = carried.count;
            const int maxStack = ItemRegistry::Get(carried.itemId).maxStackSize;
            int remaining = totalCarried;
            const int slotCount = (int)player.m_quickcraftSlots.size();

            // Per-slot distribution (MC: getQuickCraftPlaceCount line 401)
            //   type 0 (left,  split):    floor(carried / slotCount)
            //   type 1 (right, one each): 1
            //   type 2 (middle, clone):   carried (full stack to each)
            int perSlot;
            if (dragType == 0) perSlot = totalCarried / slotCount;
            else if (dragType == 1) perSlot = 1;
            else perSlot = totalCarried;

            for (uint8_t s : player.m_quickcraftSlots) {
                if (remaining <= 0 && dragType != 2) break;
                InventorySlot& target = player.getInventory().MutableSlot(s);
                int existing = target.IsEmpty() ? 0 : target.count;
                int toPlace = std::min(perSlot + existing, maxStack);
                int delta = toPlace - existing;
                if (delta <= 0) continue;
                // Component-preserving: an empty target takes a full copy of
                // the carried stack (id + components), not just the id.
                target = carried;
                target.count = toPlace;
                if (dragType != 2) remaining -= delta;
                MarkChanged(result, s);
            }

            if (dragType == 2) {
                // Clone preserves the cursor stack
            } else {
                carried.count = remaining;
                if (carried.count <= 0) carried.Clear();
            }
            result.carriedChanged = true;
            reset();
            return result;
        }

        reset();
        return result;
    }

    // ─── PICKUP_ALL (double-click) ────────────────────────────────────────
    // MC lines 547-595. Cursor non-empty + clicked slot empty/non-pickup → fill cursor by
    // collecting matching items from inventory.
    InventoryClickResult InventoryClickHandler::HandlePickupAll(ServerPlayer& player, int16_t /*slotIndex*/) {
        InventoryClickResult result;
        InventorySlot& carried = player.getCarried();
        if (carried.IsEmpty()) return result;

        Inventory& inv = player.getInventory();
        const int begin = Inventory::MAIN_BEGIN;
        const int end   = Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE;
        const int maxStack = ItemRegistry::Get(carried.itemId).maxStackSize;

        // Two-pass collect: partial stacks first (so full stacks remain), then full stacks.
        for (int pass = 0; pass < 2 && carried.count < maxStack; ++pass) {
            for (int i = begin; i < end && carried.count < maxStack; ++i) {
                InventorySlot& s = inv.MutableSlot(i);
                if (s.IsEmpty() || s.itemId != carried.itemId) continue;
                if (pass == 0 && s.count >= maxStack) continue; // pass 0: partials only
                int free = maxStack - carried.count;
                int take = std::min(free, s.count);
                carried.count += take;
                s.count -= take;
                if (s.count <= 0) s.Clear();
                MarkChanged(result, (uint8_t)i);
            }
        }
        result.carriedChanged = true;
        return result;
    }

    // ─── CREATIVE DESTROY ALL (shift-click on the trash slot) ──────────────
    // Mirrors CreativeModeInventoryScreen.slotClicked() lines 189-193: iterates
    // every slot in the player's container and sets it to ItemStack.EMPTY. Also
    // clears the cursor for parity with MC's behavior (the slotClicked() handler
    // doesn't touch the cursor here, but a fresh inventory should be paired with
    // an empty cursor — otherwise the user is left holding the carried stack and
    // promptly drops it elsewhere).
    InventoryClickResult InventoryClickHandler::HandleCreativeDestroyAll(ServerPlayer& player) {
        InventoryClickResult result;
        Inventory& inv = player.getInventory();
        // Mark every slot as changed (regardless of whether the server thought it was
        // already empty). The client may have client-side-predicted picks-ups that the
        // server never recorded — those would otherwise leave ghost stacks visible
        // because no SetSlot deltas would be sent for them. Forcing a full broadcast
        // keeps the client's view authoritative.
        for (int i = 0; i < Inventory::TOTAL_SIZE; ++i) {
            inv.MutableSlot(i).Clear();
            MarkChanged(result, (uint8_t)i);
        }
        // Clear the cursor unconditionally for the same reason — even if the server
        // thinks the cursor is empty, the client may be showing a predicted carried
        // stack from an earlier pickup the server didn't process.
        player.setCarried(InventorySlot{});
        result.carriedChanged = true;
        return result;
    }

    // ─── CREATIVE PICKUP (Search tab grid) ─────────────────────────────────
    // The search grid is an infinite source. Left-click fills cursor with full stack
    // of `itemId`. Right-click adds 1 if cursor is same item (or sets count=1 if empty).
    InventoryClickResult InventoryClickHandler::HandleCreativePickup(ServerPlayer& player,
                                                                      const Game::ItemStack& source,
                                                                      uint8_t button) {
        InventoryClickResult result;
        const uint32_t itemId = source.itemId;
        if (itemId == Game::Items::Air) return result;
        const int maxStack = Game::ItemRegistry::Get(itemId).maxStackSize;
        InventorySlot& carried = player.getCarried();

        // If the cursor is already holding a DIFFERENT item, clicking the
        // search grid just clears the cursor (the held item is discarded as
        // requested) and does NOT pick up the new item. Two clicks are needed
        // to swap: first click drops what you're holding, second click picks
        // up the new item. This makes the search-grid the de-facto "delete"
        // gesture for whatever you grabbed from your inventory.
        if (!carried.IsEmpty() && carried.itemId != itemId) {
            carried.Clear();
            result.carriedChanged = true;
            return result;
        }

        if (button == 0) {
            // Full-stack pickup (only reached when cursor is empty or same item).
            carried = source;           // identity copy — components ride along
            carried.count = maxStack;
        } else {
            // Single-item pickup (button==1).
            if (carried.IsEmpty()) {
                carried = source;
                carried.count = 1;
            } else if (carried.count < maxStack) {
                // Same item already held → increment by 1.
                carried.count++;
            }
        }
        result.carriedChanged = true;
        return result;
    }

    // ─── CREATIVE QUICK_MOVE ────────────────────────────────────────────────
    // Shift-click on the search grid: drop a full stack into hotbar (then main).
    InventoryClickResult InventoryClickHandler::HandleCreativeQuickMove(ServerPlayer& player,
                                                                        const Game::ItemStack& source) {
        InventoryClickResult result;
        const uint32_t itemId = source.itemId;
        if (itemId == Game::Items::Air) return result;
        const int maxStack = Game::ItemRegistry::Get(itemId).maxStackSize;

        Inventory& inv = player.getInventory();

        // Look for an empty hotbar slot first, then main
        const int regions[2][2] = {
            { Inventory::HOTBAR_BEGIN, Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE },
            { Inventory::MAIN_BEGIN,   Inventory::MAIN_BEGIN + Inventory::MAIN_SIZE     },
        };
        for (auto& r : regions) {
            for (int i = r[0]; i < r[1]; ++i) {
                InventorySlot& s = inv.MutableSlot(i);
                if (s.IsEmpty()) {
                    s = source;          // identity copy — components ride along
                    s.count = maxStack;
                    MarkChanged(result, (uint8_t)i);
                    return result;
                }
            }
        }
        // No empty slot — try to merge into existing same-item stacks
        for (auto& r : regions) {
            for (int i = r[0]; i < r[1]; ++i) {
                InventorySlot& s = inv.MutableSlot(i);
                if (s.itemId == itemId && s.count < maxStack) {
                    s.count = maxStack;
                    MarkChanged(result, (uint8_t)i);
                    return result;
                }
            }
        }
        return result;
    }

    InventoryClickResult InventoryClickHandler::HandleCreativeFillSlot(
            ServerPlayer& player, int16_t slotIndex, const Game::ItemStack& source) {
        InventoryClickResult result;
        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;
        if (source.itemId == Game::Items::Air) {
            // Treat Air as "clear the slot" — matches MC pick-block on an air block.
            InventorySlot& s = player.getInventory().MutableSlot(slotIndex);
            if (!s.IsEmpty()) {
                s.Clear();
                MarkChanged(result, (uint8_t)slotIndex);
            }
            return result;
        }
        // Same placement filter as regular clicks — pick-block onto an armor
        // slot must not bypass the EQUIPPABLE check.
        if (!MayPlaceInSlot(slotIndex, source)) return result;
        const int maxStack = ItemRegistry::Get(source.itemId).maxStackSize;
        InventorySlot& s = player.getInventory().MutableSlot(slotIndex);
        s = source;          // identity copy — components ride along
        s.count = maxStack;
        MarkChanged(result, (uint8_t)slotIndex);
        return result;
    }

    // ─── DISPATCH ──────────────────────────────────────────────────────────
    InventoryClickResult InventoryClickHandler::Handle(ServerPlayer& player,
                                                       const Network::InventoryClickC2SPacket& click) {
        const auto action = static_cast<Network::ContainerInput>(click.action);
        const int16_t slot = click.slotIndex;

        // Drag state must be reset if the user does anything other than continuing the drag
        // (mirrors MC line 415-416).
        if (action != Network::ContainerInput::QUICK_CRAFT && player.m_quickcraftStatus != 0) {
            player.m_quickcraftStatus = 0;
            player.m_quickcraftSlots.clear();
        }

        // Search-tab creative-grid clicks. Prefer the full creativeStack
        // (carries per-stack components, e.g. enchanted-book variants); fall
        // back to the bare creativeItemId for old-format packets — item-level
        // defaults still apply via ItemStack::get's fallback.
        const Game::ItemStack creativeSource =
            !click.creativeStack.IsEmpty()
                ? click.creativeStack
                : Game::ItemStack{static_cast<Game::ItemID>(click.creativeItemId), 1};
        if (slot == Network::InventorySlotSentinel::CREATIVE_GRID) {
            switch (action) {
                case Network::ContainerInput::PICKUP:
                    return HandleCreativePickup(player, creativeSource, click.button);
                case Network::ContainerInput::QUICK_MOVE:
                    return HandleCreativeQuickMove(player, creativeSource);
                case Network::ContainerInput::CLONE: {
                    // Middle click on creative source: same as left-click pickup
                    return HandleCreativePickup(player, creativeSource, 0);
                }
                default:
                    return {};
            }
        }

        switch (action) {
            case Network::ContainerInput::PICKUP:      return HandlePickup     (player, slot, click.button);
            case Network::ContainerInput::QUICK_MOVE:  return HandleQuickMove  (player, slot, click.button);
            case Network::ContainerInput::SWAP:        return HandleSwap       (player, slot, click.button);
            case Network::ContainerInput::CLONE:       return HandleClone      (player, slot);
            case Network::ContainerInput::THROW:       return HandleThrow      (player, slot, click.button);
            case Network::ContainerInput::QUICK_CRAFT: return HandleQuickCraft (player, slot, click.button);
            case Network::ContainerInput::PICKUP_ALL:  return HandlePickupAll  (player, slot);
            case Network::ContainerInput::CREATIVE_DESTROY_ALL: return HandleCreativeDestroyAll(player);
            case Network::ContainerInput::CREATIVE_FILL_SLOT:
                return HandleCreativeFillSlot(player, slot, creativeSource);
            default:
                Log::Warning("[InventoryClickHandler] Unknown action %u", (unsigned)click.action);
                return {};
        }
    }

} // namespace Server
