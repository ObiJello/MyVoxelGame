// File: src/common/inventory/Slot.hpp
//
// Mirrors net.minecraft.world.inventory.Slot — one clickable position in a
// menu. A Slot is a (container, containerIndex) pair plus a GUI position and
// the per-slot policy the click code consults: may this stack be placed here,
// may it be taken, and how many fit.
//
// Two index spaces meet here and must not be confused:
//   • `index`         — the slot's position in its MENU's slot list. This is
//                       what travels on the wire in InventoryClickC2SPacket.
//   • `containerSlot` — the index INSIDE the backing container.
// For the player's InventoryMenu the two happen to be equal (Game::Inventory's
// 46-slot layout already matches MC's InventoryMenu slot order), which is what
// let the menu abstraction be introduced without a wire or save-format change.
// They diverge as soon as a menu spans two containers (a chest menu's slot 27
// is the player inventory's slot 9).
#pragma once

#include "Container.hpp"
#include "common/entity/Item.hpp"
#include <algorithm>   // std::min in GetMaxStackSize

namespace Game {

    class Slot {
    public:
        Slot(IContainer* container, int containerSlot, int x, int y)
            : container(container), containerSlot(containerSlot), x(x), y(y) {}
        virtual ~Slot() = default;

        IContainer* container = nullptr;
        int containerSlot = 0;
        int index = 0;          // menu index — assigned by AbstractContainerMenu::AddSlot
        int x = 0;              // GUI position, panel-image-relative
        int y = 0;

        // ── Contents ──────────────────────────────────────────────────────
        const ItemStack& GetItem() const { return container->GetItem(containerSlot); }
        ItemStack&       GetItemMut()    { return container->GetItem(containerSlot); }
        bool             HasItem() const { return !GetItem().IsEmpty(); }

        void Set(const ItemStack& stack) {
            container->SetItem(containerSlot, stack);
            SetChanged();
        }
        // MC distinguishes set() from setByPlayer() so subclasses can react to
        // a player-driven write (crafting result slots consume ingredients).
        // Nothing overrides it yet; the distinction is kept so ported MC code
        // stays recognisable.
        virtual void SetByPlayer(const ItemStack& stack) { Set(stack); }
        void SetChanged() { container->SetChanged(); }

        // ── Policy ────────────────────────────────────────────────────────
        // MC Slot.mayPlace — may this stack be inserted here?
        virtual bool MayPlace(const ItemStack& /*stack*/) const { return true; }
        // MC Slot.mayPickup — may the contents be taken out?
        virtual bool MayPickup() const { return true; }

        // MC Slot.isActive — is this slot drawn and clickable? The 2x2 crafting
        // grid and its result exist in the 46-slot layout for MC save
        // compatibility but are not rendered by our inventory screen, so they
        // report false and the screen's layout/hit-test skip them.
        virtual bool IsActive() const { return true; }

        // MC Slot.getMaxStackSize() / getMaxStackSize(ItemStack).
        virtual int GetMaxStackSize() const { return container->GetMaxStackSize(); }
        int GetMaxStackSize(const ItemStack& stack) const {
            return std::min(GetMaxStackSize(),
                            ItemRegistry::Get(stack.itemId).maxStackSize);
        }

        // MC Slot.safeInsert(stack, amount) — move up to `amount` from `input`
        // into this slot, respecting mayPlace and the stack limit. `input` is
        // shrunk by however much was taken. Returns the number moved.
        int SafeInsert(ItemStack& input, int amount);
        int SafeInsert(ItemStack& input) { return SafeInsert(input, input.count); }

        // MC Slot.safeTake(amount, maxAmount) — remove up to min(amount,
        // maxAmount) items and return them, honouring mayPickup. The returned
        // stack keeps this slot's components.
        ItemStack SafeTake(int amount, int maxAmount);
    };

    // MC ArmorSlot — accepts exactly the piece whose EQUIPPABLE component maps
    // to this slot, and holds one of it.
    class ArmorSlot : public Slot {
    public:
        using Slot::Slot;
        bool MayPlace(const ItemStack& stack) const override;
        int  GetMaxStackSize() const override { return 1; }
    };

    // Refuses every insert. Stands in for the 2x2 crafting grid and its result
    // slot, which exist in the 46-slot layout for MC save compatibility but
    // have no crafting system behind them yet. Contents can still be taken out
    // (MC's result slot behaves the same way), so anything already sitting
    // there from a loaded world can be retrieved.
    class NoPlaceSlot : public Slot {
    public:
        using Slot::Slot;
        bool MayPlace(const ItemStack& /*stack*/) const override { return false; }
        bool IsActive() const override { return false; }
    };

} // namespace Game
