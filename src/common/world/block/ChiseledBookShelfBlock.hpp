// File: src/common/world/block/ChiseledBookShelfBlock.hpp
//
// Mirrors net.minecraft.world.level.block.ChiseledBookShelfBlock: a book
// (#bookshelf_books) used on the front face goes into the slot under the
// cursor (SelectableSlotContainer.getHitSlot: 3 columns × 2 rows of the
// face), an empty hand takes that slot's book; the comparator reads the last
// slot touched + 1; breaking it notifies comparators. The books live in
// ChiseledBookShelfBlockEntity.
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    void RegisterChiseledBookShelfBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
