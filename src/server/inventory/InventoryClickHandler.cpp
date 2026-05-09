// File: src/server/inventory/InventoryClickHandler.cpp
// Mirrors net.minecraft.world.inventory.AbstractContainerMenu.doClick().
#include "InventoryClickHandler.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
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

    bool InventoryClickHandler::IsRestrictedInsertSlot(int16_t slotIndex) {
        // Armor + crafting are visual-only in our scope. Block placement into them.
        return Inventory::IsArmorSlot(slotIndex) || Inventory::IsCraftGridSlot(slotIndex)
            || Inventory::IsCraftResultSlot(slotIndex);
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
                    // Right-click outside drops 1
                    result.droppedItem = {carried.itemId, 1};
                    carried.count--;
                    if (carried.count <= 0) carried.Clear();
                }
                result.carriedChanged = true;
            }
            return result;
        }

        if (slotIndex < 0 || slotIndex >= Inventory::TOTAL_SIZE) return result;

        InventorySlot& slot = inv.MutableSlot(slotIndex);

        // Visual-only slots (armor/craft) — let cursor pick up if non-empty, refuse insert.
        const bool restricted = IsRestrictedInsertSlot(slotIndex);

        if (slot.IsEmpty()) {
            if (carried.IsEmpty()) return result;
            if (restricted) return result;
            // Insert from cursor
            const int maxStack = ItemRegistry::Get(carried.itemId).maxStackSize;
            int amount = (button == 0) ? carried.count : 1;
            amount = std::min(amount, maxStack);
            slot = {carried.itemId, amount};
            carried.count -= amount;
            if (carried.count <= 0) carried.Clear();
            result.carriedChanged = true;
            MarkChanged(result, (uint8_t)slotIndex);
            return result;
        }

        // Slot is non-empty
        if (carried.IsEmpty()) {
            // Pick up: left = full stack, right = ceil(count/2)
            int amount = (button == 0) ? slot.count : (slot.count + 1) / 2;
            carried = {slot.itemId, amount};
            slot.count -= amount;
            if (slot.count <= 0) slot.Clear();
            result.carriedChanged = true;
            MarkChanged(result, (uint8_t)slotIndex);
            return result;
        }

        // Both non-empty
        if (slot.itemId == carried.itemId) {
            // Same item: merge cursor → slot
            if (restricted) return result;
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
            // Different blocks: swap (only if mayPlace; restricted slots refuse swap)
            if (restricted) return result;
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

        if (Inventory::IsHotbarSlot(slotIndex)) {
            // Hotbar → main
            MoveStackToRegion(inv, slotIndex, Inventory::MAIN_BEGIN,
                              Inventory::MAIN_BEGIN + Inventory::MAIN_SIZE, result);
        } else if (Inventory::IsMainSlot(slotIndex)) {
            // Main → hotbar
            MoveStackToRegion(inv, slotIndex, Inventory::HOTBAR_BEGIN,
                              Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE, result);
        } else if (Inventory::IsArmorSlot(slotIndex) ||
                   Inventory::IsCraftGridSlot(slotIndex) ||
                   Inventory::IsCraftResultSlot(slotIndex)) {
            // Restricted source → try main first, then hotbar
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

        // Restricted slot can only be picked up (placed *into* hotbar). Don't insert from hotbar.
        if (IsRestrictedInsertSlot(slotIndex) && !b.IsEmpty()) return result;

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

        carried = {src.itemId, ItemRegistry::Get(src.itemId).maxStackSize};
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
                    result.droppedItem = {carried.itemId, 1};
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
        result.droppedItem = {slot.itemId, amount};
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
            if (IsRestrictedInsertSlot(slotIndex)) return result;
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
                target.itemId = carried.itemId;
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
                                                                      uint32_t itemId, uint8_t button) {
        InventoryClickResult result;
        if (itemId == Game::Items::Air) return result;
        const int maxStack = Game::ItemRegistry::Get(itemId).maxStackSize;
        InventorySlot& carried = player.getCarried();

        if (button == 0) {
            // Left click: full stack
            carried = {itemId, maxStack};
        } else {
            // Right click: +1 (if same item) or set to 1
            if (carried.IsEmpty()) {
                carried = {itemId, 1};
            } else if (carried.itemId == itemId && carried.count < maxStack) {
                carried.count++;
            }
        }
        result.carriedChanged = true;
        return result;
    }

    // ─── CREATIVE QUICK_MOVE ────────────────────────────────────────────────
    // Shift-click on the search grid: drop a full stack into hotbar (then main).
    InventoryClickResult InventoryClickHandler::HandleCreativeQuickMove(ServerPlayer& player, uint32_t itemId) {
        InventoryClickResult result;
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
                    s = {itemId, maxStack};
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

        // Search-tab creative-grid clicks
        if (slot == Network::InventorySlotSentinel::CREATIVE_GRID) {
            switch (action) {
                case Network::ContainerInput::PICKUP:
                    return HandleCreativePickup(player, click.creativeItemId, click.button);
                case Network::ContainerInput::QUICK_MOVE:
                    return HandleCreativeQuickMove(player, click.creativeItemId);
                case Network::ContainerInput::CLONE: {
                    // Middle click on creative source: same as left-click pickup
                    return HandleCreativePickup(player, click.creativeItemId, 0);
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
            default:
                Log::Warning("[InventoryClickHandler] Unknown action %u", (unsigned)click.action);
                return {};
        }
    }

} // namespace Server
