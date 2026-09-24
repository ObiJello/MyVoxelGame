// File: src/common/world/block/entity/DispenserBlockEntity.cpp
#include "common/world/block/entity/DispenserBlockEntity.hpp"

#include "common/core/JavaRandom.hpp"

#include <algorithm>

namespace Game {

    int DispenserBlockEntity::GetRandomSlot(JavaRandom& random) const {
        int replaceSlot = -1;
        int replaceOdds = 1;
        for (int i = 0; i < GetContainerSize(); ++i) {
            if (!GetItem(i).IsEmpty() && random.NextInt(replaceOdds++) == 0) replaceSlot = i;
        }
        return replaceSlot;
    }

    ItemStack DispenserBlockEntity::InsertItem(ItemStack stack) {
        const int maxStackSize = GetMaxStackSize(stack);
        for (int i = 0; i < GetContainerSize(); ++i) {
            ItemStack& target = GetItem(i);
            if (target.IsEmpty() || IsSameItemSameComponents(stack, target)) {
                const int transferCount = std::min(stack.count, maxStackSize - target.count);
                if (transferCount > 0) {
                    if (target.IsEmpty()) {
                        ItemStack split = stack;
                        split.count = transferCount;
                        stack.count -= transferCount;
                        if (stack.count <= 0) stack.Clear();
                        SetItem(i, split);
                    } else {
                        stack.count -= transferCount;
                        if (stack.count <= 0) stack.Clear();
                        target.count += transferCount;
                        SetChanged();
                    }
                }
                if (stack.IsEmpty()) break;
            }
        }
        return stack;
    }

} // namespace Game
