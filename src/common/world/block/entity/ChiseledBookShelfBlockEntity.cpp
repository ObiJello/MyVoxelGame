// File: src/common/world/block/entity/ChiseledBookShelfBlockEntity.cpp
#include "ChiseledBookShelfBlockEntity.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/tags/DataTags.hpp"

namespace Game {

    bool ChiseledBookShelfBlockEntity::AcceptsItemType(const ItemStack& stack) {
        return !stack.IsEmpty() &&
               DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId), "minecraft:bookshelf_books");
    }

    void ChiseledBookShelfBlockEntity::UpdateState(int interactedSlot) {
        if (interactedSlot < 0 || interactedSlot >= kMaxBooks) {
            Log::Error("[ChiseledBookShelf] Expected slot 0-5, got %d", interactedSlot);
            return;
        }
        m_lastInteractedSlot = interactedSlot;
        ILevelWrite* level = GetLevel();
        if (!level || level->IsClientSide()) return;
        const glm::ivec3 pos = GetWorldPos();
        BlockState state = level->GetBlockState(pos.x, pos.y, pos.z);
        if (state.Block() != GetBlockId()) return;
        for (int slot = 0; slot < kMaxBooks; ++slot) {
            const bool occupied = !BaseContainerBlockEntity::GetItem(slot).IsEmpty();
            state = state.SetName(static_cast<PropertyId>(static_cast<int>(PropertyId::SLOT_0_OCCUPIED) + slot),
                                  occupied ? "true" : "false");
        }
        // level.setBlockAndUpdate + gameEvent(BLOCK_CHANGE, pos,
        // Context.of(updatedState)). The same block: the entity stays.
        level->SetBlock(pos.x, pos.y, pos.z, state, World::UpdateFlags::All);
        level->GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(state));
    }

    void ChiseledBookShelfBlockEntity::SetItem(int index, const ItemStack& stack) {
        if (AcceptsItemType(stack)) {
            BaseContainerBlockEntity::SetItem(index, stack);
            UpdateState(index);
        } else if (stack.IsEmpty()) {
            RemoveItem(index, GetMaxStackSize());
        }
    }

    ItemStack ChiseledBookShelfBlockEntity::RemoveItem(int slot, int /*count*/) {
        if (slot < 0 || slot >= kMaxBooks) return ItemStack{};
        const ItemStack retrieved = BaseContainerBlockEntity::GetItem(slot);
        BaseContainerBlockEntity::SetItem(slot, ItemStack{});
        if (!retrieved.IsEmpty()) UpdateState(slot);
        return retrieved;
    }

    bool ChiseledBookShelfBlockEntity::CanPlaceItem(int slot, const ItemStack& stack) const {
        // ListBackedContainer.canPlaceItem.
        if (!AcceptsItemType(stack)) return false;
        const ItemStack& current = BaseContainerBlockEntity::GetItem(slot);
        return current.IsEmpty() || current.count < GetMaxStackSize(stack);
    }

    bool ChiseledBookShelfBlockEntity::CanTakeItem(const IContainer& into, int /*slot*/, const ItemStack& stack) const {
        // into.hasAnyMatching(empty, or the same item with room).
        for (int i = 0; i < into.GetContainerSize(); ++i) {
            const ItemStack& to = into.GetItem(i);
            if (to.IsEmpty()) return true;
            if (IsSameItemSameComponents(stack, to) && to.count + stack.count <= into.GetMaxStackSize(to)) return true;
        }
        return false;
    }

} // namespace Game
