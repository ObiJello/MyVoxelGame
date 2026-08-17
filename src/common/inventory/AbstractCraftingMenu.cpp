// File: src/common/inventory/AbstractCraftingMenu.cpp
#include "AbstractCraftingMenu.hpp"

namespace Game {

    void CraftingResultSlot::OnTake(const ItemStack& /*taken*/, ContainerClickResult& result) {
        if (m_menu) m_menu->OnResultTaken(result);
    }

    void AbstractCraftingMenu::Configure(IContainer* craftContainer, int craftBase,
                                         IContainer* resultContainer, int resultIdx,
                                         int gridMenuBegin, int resultMenuIndex) {
        m_craftContainer  = craftContainer;
        m_craftBase       = craftBase;
        m_resultContainer = resultContainer;
        m_resultIdx       = resultIdx;
        m_gridMenuBegin   = gridMenuBegin;
        m_resultMenuIndex = resultMenuIndex;
    }

    CraftingInput AbstractCraftingMenu::BuildInput(int& outLeft, int& outTop) const {
        outLeft = 0;
        outTop  = 0;
        if (!m_craftContainer) return CraftingInput{};

        std::vector<ItemStack> cells;
        cells.reserve(static_cast<size_t>(GridSize()));
        for (int i = 0; i < GridSize(); ++i) {
            cells.push_back(m_craftContainer->GetItem(m_craftBase + i));
        }
        return CraftingInput::OfPositioned(m_gridWidth, m_gridHeight, cells, outLeft, outTop);
    }

    void AbstractCraftingMenu::SlotsChanged(ContainerClickResult& result) {
        if (!m_craftContainer || !m_resultContainer) return;

        // MC CraftingMenu.slotChangedCraftingGrid: look the grid up, assemble
        // the winner, write it into slot 0. No match writes an empty stack, so
        // removing an ingredient clears the output.
        int left = 0, top = 0;
        const CraftingInput input = BuildInput(left, top);

        ItemStack assembled{};
        if (const CraftingRecipe* recipe = RecipeManager::Find(input)) {
            assembled = RecipeManager::Assemble(*recipe, input);
        }

        const ItemStack& current = m_resultContainer->GetItem(m_resultIdx);
        if (ItemStacksMatch(current, assembled)) return;
        m_resultContainer->SetItem(m_resultIdx, assembled);
        MarkChanged(result, m_resultMenuIndex);
    }

    void AbstractCraftingMenu::OnResultTaken(ContainerClickResult& result) {
        if (!m_craftContainer) return;

        // Verbatim port of ResultSlot.onTake (ResultSlot.java:81-110): walk the
        // TRIMMED input, decrement each contributing cell by one, then put back
        // whatever the recipe leaves behind (the bucket from a milk bucket).
        int left = 0, top = 0;
        const CraftingInput input = BuildInput(left, top);
        if (input.IsEmpty()) return;

        const std::vector<ItemStack> remaining = RecipeManager::GetRemainingItems(input);
        bool spilledToInventory = false;

        for (int y = 0; y < input.Height(); ++y) {
            for (int x = 0; x < input.Width(); ++x) {
                const int cell = (x + left) + (y + top) * m_gridWidth;
                ItemStack current = m_craftContainer->GetItem(m_craftBase + cell);
                const ItemStack& replacement = remaining[static_cast<size_t>(x + y * input.Width())];

                bool cellChanged = false;
                if (!current.IsEmpty()) {
                    current.count--;
                    if (current.count <= 0) current.Clear();
                    cellChanged = true;
                }

                if (!replacement.IsEmpty()) {
                    if (current.IsEmpty()) {
                        current = replacement;
                        cellChanged = true;
                    } else if (IsSameItemSameComponents(current, replacement)) {
                        current.count += replacement.count;
                        cellChanged = true;
                    } else {
                        // The cell still holds something else (a stack of milk
                        // buckets crafting one at a time). MC pushes the
                        // remainder into the inventory and drops it if that
                        // fails; with no item entities, a full inventory loses
                        // it — same as everywhere else we would drop.
                        getInventory().AddStack(replacement);
                        spilledToInventory = true;
                    }
                }

                if (cellChanged) {
                    m_craftContainer->SetItem(m_craftBase + cell, current);
                    MarkChanged(result, m_gridMenuBegin + cell);
                }
            }
        }

        if (spilledToInventory) {
            // AddStack doesn't report where it wrote, so re-broadcast the whole
            // menu. This only happens on the remainder-doesn't-fit path.
            for (int i = 0; i < SlotCount(); ++i) MarkChanged(result, i);
        }
    }

    void AbstractCraftingMenu::Removed(ContainerClickResult& result) {
        if (!m_craftContainer) return;

        // MC clearContainer(player, craftSlots) — anything parked in the grid
        // goes back to the player rather than vanishing with the menu.
        for (int i = 0; i < GridSize(); ++i) {
            const ItemStack parked = m_craftContainer->GetItem(m_craftBase + i);
            if (parked.IsEmpty()) continue;
            m_craftContainer->SetItem(m_craftBase + i, ItemStack{});
            const int leftover = getInventory().AddStack(parked);
            if (leftover > 0) {
                // Full inventory — into the world instead of nowhere.
                ItemStack overflow = parked;
                overflow.count = leftover;
                result.extraDrops.push_back(overflow);
            }
            MarkChanged(result, m_gridMenuBegin + i);
        }

        // The output square is derived state; it never survives a close.
        if (m_resultContainer && !m_resultContainer->GetItem(m_resultIdx).IsEmpty()) {
            m_resultContainer->SetItem(m_resultIdx, ItemStack{});
            MarkChanged(result, m_resultMenuIndex);
        }
    }

} // namespace Game
