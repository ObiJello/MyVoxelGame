// File: src/common/world/block/CopperGolemStatueBlock.hpp
//
// Mirrors net.minecraft.world.level.block.CopperGolemStatueBlock and
// WeatheringCopperGolemStatueBlock: using a statue with anything but an axe
// turns it to its next pose (standing → sitting → running → star →
// standing, the become-statue sound); its comparator reading is the pose
// ordinal + 1; the unwaxed stages age like every weathering copper block
// (random tick, WeatheringCopper.changeOverTime). Axes scrape and
// honeycomb waxes through the copper families (ItemBehaviors).
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    bool IsCopperGolemStatue(BlockID id);

    void RegisterCopperGolemStatueBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
