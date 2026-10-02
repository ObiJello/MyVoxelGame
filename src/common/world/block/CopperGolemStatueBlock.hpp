// File: src/common/world/block/CopperGolemStatueBlock.hpp
//
// Mirrors net.minecraft.world.level.block.CopperGolemStatueBlock and
// WeatheringCopperGolemStatueBlock: using a statue with anything but an axe
// turns it to its next pose (standing → sitting → running → star →
// standing, the become-statue sound) — except honeycomb on an unwaxed one,
// which passes to the wax; an axe on the UNAFFECTED unwaxed statue wakes the
// copper golem it was (CopperGolemStatueBlockEntity::RemoveStatue), on any
// other stage it passes to the axe's scrape / wax-off (ItemBehaviors). Its
// comparator reading is the pose ordinal + 1; the unwaxed stages age like
// every weathering copper block (random tick, WeatheringCopper.changeOverTime).
// A fully oxidized copper golem sets into the oxidized statue on its own
// (CopperGolem::TurnToStatue).
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    bool IsCopperGolemStatue(BlockID id);

    void RegisterCopperGolemStatueBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
