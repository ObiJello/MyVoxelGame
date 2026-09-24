// File: src/common/world/block/LecternBlock.hpp
//
// Mirrors net.minecraft.world.level.block.LecternBlock — the block half of
// the lectern: placing a book on it, opening its reading menu, the two-tick
// redstone pulse on a page turn, and the comparator reading of the open page.
// The book itself lives in LecternBlockEntity.
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;

    // Wires useItemOn / useWithoutItem / the signal hooks / tick /
    // affectNeighborsAfterRemoval onto BlockID::Lectern. Called from
    // RegisterBlockBehaviors after the container wiring.
    void RegisterLecternBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

    // MC LecternBlock.resetBookState: POWERED off, HAS_BOOK = `hasBook`, a
    // full update, and the block below told (a lectern powers the block it
    // stands on through getDirectSignal(UP)).
    void LecternResetBookState(ILevelWrite& level, const glm::ivec3& pos, BlockState state, bool hasBook);

    // MC LecternBlock.signalPageChange: POWERED on for two ticks (the
    // scheduled tick turns it off again) and the page-turn sound (level event
    // 1043).
    void LecternSignalPageChange(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

} // namespace Game
