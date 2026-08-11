// File: src/common/inventory/SimpleContainer.hpp
//
// Mirrors net.minecraft.world.SimpleContainer — a plain fixed-size stack array
// with no behaviour of its own. Menus that own storage the player inventory
// does not (a crafting table's 3x3 grid and its output square) build their
// slots over one of these.
//
// MC calls the crafting variants TransientCraftingContainer / ResultContainer;
// the only thing that distinguishes them there is a back-pointer used to notify
// the menu, and our menus recompute from AbstractContainerMenu::SlotsChanged
// instead — so one type covers both.
#pragma once

#include "Container.hpp"
#include <vector>

namespace Game {

    class SimpleContainer : public IContainer {
    public:
        explicit SimpleContainer(int size) : m_items(static_cast<size_t>(size)) {}

        int GetContainerSize() const override { return static_cast<int>(m_items.size()); }

        ItemStack& GetItem(int index) override {
            static ItemStack scratch{};
            if (index < 0 || index >= GetContainerSize()) { scratch = ItemStack{}; return scratch; }
            return m_items[static_cast<size_t>(index)];
        }
        const ItemStack& GetItem(int index) const override {
            static const ItemStack kEmpty{};
            if (index < 0 || index >= GetContainerSize()) return kEmpty;
            return m_items[static_cast<size_t>(index)];
        }
        void SetItem(int index, const ItemStack& stack) override {
            if (index < 0 || index >= GetContainerSize()) return;
            m_items[static_cast<size_t>(index)] = stack;
        }

    private:
        std::vector<ItemStack> m_items;
    };

} // namespace Game
