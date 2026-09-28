// File: src/common/world/block/entity/ChiseledBookShelfBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.ChiseledBookShelfBlockEntity
// — six book slots (ListBackedContainer, max stack 1, only #bookshelf_books)
// and the last slot a book went into or came out of, which is the shelf's
// comparator reading (+1). Every insertion or removal rewrites the block's
// slot_N_occupied properties (updateState) — the model shows the books.
//
// Being a container it spills its books when broken or blown up, takes and
// gives books through hoppers, and saves Items + last_interacted_slot.
#pragma once

#include "BaseContainerBlockEntity.hpp"

namespace Game {

    class ChiseledBookShelfBlockEntity : public BaseContainerBlockEntity {
    public:
        static constexpr int kMaxBooks = 6;   // MC MAX_BOOKS_IN_STORAGE

        ChiseledBookShelfBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, kMaxBooks) {}

        // MC acceptsItemType: #minecraft:bookshelf_books.
        static bool AcceptsItemType(const ItemStack& stack);

        int  GetLastInteractedSlot() const { return m_lastInteractedSlot; }
        void SetLastInteractedSlot(int slot) { m_lastInteractedSlot = slot; }

        // ── IContainer (ListBackedContainer + the overrides) ─────────────
        using IContainer::GetMaxStackSize;
        int  GetMaxStackSize() const override { return 1; }
        void SetItem(int index, const ItemStack& stack) override;
        ItemStack RemoveItem(int slot, int count) override;
        bool CanPlaceItem(int slot, const ItemStack& stack) const override;
        bool CanTakeItem(const IContainer& into, int slot, const ItemStack& stack) const override;

    private:
        // MC updateState: the occupied properties follow the slots.
        void UpdateState(int interactedSlot);

        int m_lastInteractedSlot = -1;
    };

} // namespace Game
