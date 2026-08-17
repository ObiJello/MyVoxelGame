// File: src/common/inventory/CompoundContainer.hpp
//
// Mirrors net.minecraft.world.CompoundContainer — two containers presented as
// one. A double chest is not a 54-slot block entity: it stays two 27-slot
// chests, and this joins them so a single menu can span both.
//
// Slot mapping is MC's exactly (CompoundContainer.java:28-33): indices below
// container1's size address container1, the rest address container2 offset by
// that size. Which chest is "first" therefore decides which half of the screen
// shows which chest's items — see DoubleChest.hpp for how that is resolved.
#pragma once

#include "Container.hpp"

namespace Game {

    class CompoundContainer : public IContainer {
    public:
        // Both containers are BORROWED — they are the two block entities living
        // in the chunk. The menu owning this must not outlive either, which is
        // what PlayerSession's stillValid check enforces for both halves.
        CompoundContainer(IContainer* first, IContainer* second)
            : m_first(first), m_second(second) {}

        int GetContainerSize() const override {
            return m_first->GetContainerSize() + m_second->GetContainerSize();
        }

        ItemStack& GetItem(int index) override {
            const int split = m_first->GetContainerSize();
            return index >= split ? m_second->GetItem(index - split) : m_first->GetItem(index);
        }
        const ItemStack& GetItem(int index) const override {
            const int split = m_first->GetContainerSize();
            return index >= split ? m_second->GetItem(index - split) : m_first->GetItem(index);
        }
        void SetItem(int index, const ItemStack& stack) override {
            const int split = m_first->GetContainerSize();
            if (index >= split) m_second->SetItem(index - split, stack);
            else                m_first->SetItem(index, stack);
        }

        // MC CompoundContainer.setChanged marks BOTH halves — a menu-level
        // change can't be attributed to one chest, and a missed dirty bit means
        // the contents never save.
        void SetChanged() override {
            m_first->SetChanged();
            m_second->SetChanged();
        }

        int GetMaxStackSize() const override { return m_first->GetMaxStackSize(); }

    private:
        IContainer* m_first  = nullptr;
        IContainer* m_second = nullptr;
    };

} // namespace Game
