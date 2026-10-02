// File: src/common/world/block/CarvedPumpkinBlock.hpp
//
// MC net.minecraft.world.level.block.CarvedPumpkinBlock (26.3) — the carved
// pumpkin and the jack o'lantern (both are CarvedPumpkinBlocks in vanilla)
// and the three golems they finish:
//
//     snow golem     "^"        iron golem   "~^~"      copper golem   "^"
//                    "#"  snow                "###" iron                 "#" #copper
//                    "#"  snow                "~#~"
//
//   ^ = carved_pumpkin or jack_o_lantern (any facing)
//   ~ = air (BlockStateBase.isAir), so an iron golem's arms must stand clear
//
// matched by BlockPattern::Find in any orientation. onPlace runs the check
// however the head got there — a player, a dispenser (which only places a
// pumpkin where CanSpawnGolem says a golem will result), a piston, an
// enderman — because it hangs off World::SetBlock's onPlace step. The check
// is server-only (MC LevelChunk.setBlockState never calls onPlace on the
// client); clients see the cleared blocks as ordinary block updates and the
// golem as an ordinary entity spawn.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/pattern/BlockPattern.hpp"

#include <glm/glm.hpp>

#include <array>

namespace Game {

    struct IBlockAccess;
    class  ILevelWrite;

    // MC CarvedPumpkinBlock.onPlace: a head placed where there was not
    // already one tries to finish a golem (trySpawnGolem).
    void CarvedPumpkinOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState,
                              BlockState oldState, bool movedByPiston);

    // MC CarvedPumpkinBlock.canSpawnGolem: would a head at `topPos` finish a
    // snow, iron or copper golem (the patterns without their head)? The
    // dispenser's carved-pumpkin behaviour asks before it places one.
    bool CarvedPumpkinCanSpawnGolem(const IBlockAccess& level, const glm::ivec3& topPos);

    // MC CarvedPumpkinBlock.clearPatternBlocks: every cell of the match's
    // front aisle becomes air with flag 2 (clients only — no neighbour
    // updates yet), each with level event 2001 for the block it held (break
    // particles and sound). Shared with the wither ritual
    // (WitherSkullBlock.checkSpawn calls it too).
    void ClearPatternBlocks(ILevelWrite& level, const BlockPattern::Match& match);

    // MC CarvedPumpkinBlock.updatePatternBlocks: the deferred neighbour
    // updates for those cells (updateNeighborsAt(pos, AIR)), once the mob
    // exists.
    void UpdatePatternBlocks(ILevelWrite& level, const BlockPattern::Match& match);

    // Wires onPlace on the carved pumpkin and the jack o'lantern. Called from
    // BlockRegistry_RegisterBehaviors.
    void RegisterCarvedPumpkinBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
