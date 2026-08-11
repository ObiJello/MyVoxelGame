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

    struct ContainerClickResult;   // AbstractContainerMenu.hpp — see OnTake

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

        // MC Slot.getNoItemIcon — the placeholder sprite drawn when this slot
        // is empty (the armour silhouettes, the shield outline). Null for
        // ordinary slots. It lives on the SLOT rather than being derived from
        // the index by the screen, because the same index means different
        // things in different menus: menu slot 5 is a leggings slot in the
        // player's menu and the middle of the grid in a crafting table's.
        const char* noItemIcon = nullptr;

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

        // MC Slot.isActive — is this slot drawn and clickable at all? A screen
        // skips inactive slots in both its layout and its hit-test. Every slot
        // in InventoryMenu is active; the creative screen hides the crafting
        // ones by overriding their position instead, exactly as MC does.
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

        // MC Slot.onTake — the player has just removed `taken` from this slot.
        // A crafting result slot uses it to consume one set of ingredients from
        // its grid; every slot it touches must be recorded on `result` so the
        // change reaches the client. Called from the four click paths that can
        // empty a slot (PICKUP, QUICK_MOVE, SWAP, THROW), matching MC.
        virtual void OnTake(const ItemStack& taken, ContainerClickResult& result) {
            (void)taken;
            (void)result;
        }
    };

    // MC ArmorSlot — accepts exactly the piece whose EQUIPPABLE component maps
    // to this slot, and holds one of it.
    class ArmorSlot : public Slot {
    public:
        using Slot::Slot;
        bool MayPlace(const ItemStack& stack) const override;
        int  GetMaxStackSize() const override { return 1; }
    };

    // The crafting output square lives in AbstractCraftingMenu.hpp as
    // CraftingResultSlot — it needs the menu to consume the grid on take, which
    // this header cannot see.

} // namespace Game
