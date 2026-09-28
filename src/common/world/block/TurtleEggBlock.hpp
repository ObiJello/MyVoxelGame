// File: src/common/world/block/TurtleEggBlock.hpp
//
// Mirrors net.minecraft.world.level.block.TurtleEggBlock (26.3):
//
//   * 1..4 eggs in one cell (EGGS; placement stacking is BlockPlacement's
//     StackedPlacementState) and three hatch stages (HATCH 0..2, the
//     blockstate's cracked models);
//   * on sand, a random tick cracks an egg one stage — always in the
//     pre-dawn window, one in 500 otherwise (TURTLE_EGG_HATCH_CHANCE) — and
//     the third crack hatches every egg into a baby turtle that calls the
//     spot home;
//   * a walking entity crushes an egg one tick in a hundred unless it
//     sneaks (stepOn), a landing one in three unless it is a zombie
//     (fallOn); turtles and bats never do, other mobs only under
//     mobGriefing; a player breaking a clutch takes one egg at a time;
//   * laid or placed on sand, the eggs give off the growth sparkle (level
//     event 2012).
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;

    namespace TurtleEgg {

        // MC TurtleEggBlock.playerDestroy's decreaseEggs: after a survival
        // player breaks the block, a clutch of more than one comes back one
        // egg smaller. `state` is the state that was broken.
        void PlayerDestroy(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

    } // namespace TurtleEgg

    // Wires the turtle egg's hooks. Called from BlockRegistry_RegisterBehaviors.
    void RegisterTurtleEggBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
