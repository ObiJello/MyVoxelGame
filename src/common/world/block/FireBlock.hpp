// File: src/common/world/block/FireBlock.hpp
//
// MC net.minecraft.world.level.block.FireBlock (and the survival halves of
// BaseFireBlock / SoulFireBlock): the fire's scheduled tick — ageing,
// burning out, burning its fuel away, spreading, and going out in the rain —
// the flammability table (FireBlock.bootStrap, GeneratedFireFlammability.inc),
// the placement state with its side faces, and canSurvive / updateShape.
//
// The tick, per MC FireBlock.tick (every 30..39 ticks, re-booked first):
//   * only where fire may spread (canSpreadFireAround — the
//     fire_spread_radius_around_player rule);
//   * a fire that cannot survive is removed (and goes on ticking this once);
//   * off infiniburn (the dimension's #infiniburn_* under it), a fire in the
//     rain — raining, and rain at it or one of its four horizontal
//     neighbours (isNearRain) — goes out with 0.2 + age x 0.03 chance;
//   * otherwise it ages (0..15), dies without fuel, burns its neighbours
//     (checkBurnOut, priming TNT; a replaced block only re-lights when it is
//     not being rained on) and spreads into the air around it (a 3x3x6
//     box) — never into a cell near rain while it rains. Humid biomes
//     (INCREASED_FIRE_BURNOUT: jungle, swamp, mushroom fields, the peaks …)
//     burn out faster and spread half as far.
#pragma once

#include "BlockRegistry.hpp"
#include "BlockState.hpp"

#include <glm/glm.hpp>

#include <array>

namespace Game {

    struct IBlockAccess;
    class  ILevelWrite;
    struct EntityLevel;

    // MC FireBlock.getIgniteOdds(state) / getBurnOdds(state): the table's
    // value, 0 for a waterlogged block or one not in it.
    int  FireIgniteOdds(BlockState state);
    int  FireBurnOdds(BlockState state);
    // MC FireBlock.canBurn — ignite odds above zero.
    bool FireCanBurn(BlockState state);

    // MC BaseFireBlock.getState(level, pos): soul fire on a soul-fire base,
    // otherwise FireBlock.getStateForPlacement — on a burnable or sturdy
    // floor the plain fire, else a fire clinging to each burnable side.
    BlockState FireStateForPlacement(const IBlockAccess& level, const glm::ivec3& pos);

    // MC FireBlock.canSurvive / SoulFireBlock.canSurvive.
    bool FireBlockCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state);

    // MC ServerLevel.canSpreadFireAround(pos).
    bool FireCanSpreadAround(const EntityLevel& level, const glm::ivec3& pos);

    // MC FireBlock.onPlace's `scheduleTick(pos, this, getFireTickDelay())`
    // (30..39) — every placement, the portal attempt's included. Called from
    // the fire's onPlace (BlockBehaviors' FireOnPlace).
    void FireScheduleTick(ILevelWrite& level, const glm::ivec3& pos);

    // Wires the fire's tick and updateShape, and the soul fire's updateShape.
    // Called from BlockRegistry_RegisterBehaviors.
    void RegisterFireBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
