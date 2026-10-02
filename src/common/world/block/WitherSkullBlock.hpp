// File: src/common/world/block/WitherSkullBlock.hpp
//
// MC net.minecraft.world.level.block.WitherSkullBlock (26.3) — the wither
// skeleton skull, floor and wall (WitherWallSkullBlock defers to it), and the
// soul-sand ritual it finishes:
//
//     "^^^"      ^ = wither skeleton skull, floor or wall, any rotation
//     "###"      # = #wither_summon_base_blocks (soul sand, soul soil)
//     "~#~"      ~ = air (BlockStateBase.isAir)
//
// matched by BlockPattern::Find in any orientation, in whichever level the
// skull went into. The check runs from exactly MC's two trigger points: the
// skull's setPlacedBy (a player's BlockItem.place — never onPlace, so a
// skull a piston or a structure puts down does not summon, and completing
// the T with the sand does not either) and the dispenser's wither-skull
// behaviour, which only places a skull where canSpawnMob says the wither
// will result.
#pragma once

#include "common/world/block/BlockRegistry.hpp"

#include <glm/glm.hpp>

#include <array>

namespace Game {

    class  ILevelWrite;
    struct ItemStack;

    // MC WitherSkullBlock.checkSpawn(level, pos): server side, the cell
    // holds a wither skull (floor or wall), pos.y >= the level's min Y, the
    // difficulty is not PEACEFUL, and the full pattern matches — then the
    // pattern is cleared (CarvedPumpkinBlock.clearPatternBlocks), a wither is
    // put at its base (+0.5, +0.55, +0.5) yawed along the pattern plane,
    // made invulnerable (the 220-tick charge), SUMMONED_ENTITY goes to every
    // player within 50 of it, it is added, and the deferred neighbour
    // updates run.
    void WitherSkullCheckSpawn(ILevelWrite& level, const glm::ivec3& pos);

    // MC WitherSkullBlock.canSpawnMob(level, pos, stack): a wither skeleton
    // skull ITEM, pos.y >= minY + 2, not PEACEFUL, server side, and the
    // pattern minus its skulls matches at `pos`.
    bool WitherSkullCanSpawnMob(ILevelWrite& level, const glm::ivec3& pos, const ItemStack& stack);

    // Wires setPlacedBy on the wither skeleton skull and wall skull. Called
    // from BlockRegistry_RegisterBehaviors.
    void RegisterWitherSkullBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
