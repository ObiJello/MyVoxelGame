// File: src/common/world/fluid/FlowingFluid.hpp
//
// Port of MC's FlowingFluid / WaterFluid / LavaFluid (the spreading
// simulation) and LiquidBlock (the block hooks that book it). This is the
// server half of fluids; FluidState.hpp is the read-only model both sides
// share.
//
// Shape of the port, and how it maps onto this engine's tick machinery:
//
//   * MC keeps fluid ticks in their own LevelTicks<Fluid> queue, drained
//     right after the block ticks. Here they ride the ONE scheduled-tick
//     queue, keyed on BlockID::Water / BlockID::Lava. That is the same
//     identity vanilla's queue has — (pos, water) — because a fluid tick
//     names the fluid, never the block holding it: a waterlogged fence
//     books a `water` tick on its own cell. World::ProcessBlockUpdates
//     therefore dispatches those two ids on the cell's FLUID state, not its
//     block (ServerLevel.tickFluid's `fluidState.is(type)` guard), and the
//     Anvil serializer writes them to `fluid_ticks` so vanilla reads them.
//
//   * A tick fires FlowingFluid.tick: re-derive this cell from its
//     neighbours (getNewLiquid), rewrite it if that changed, then spread —
//     down first, then to whichever sides are nearest a drop
//     (getSpread / getSlopeDistance, water looks 4 blocks, lava 2).
//
//   * Spreading is booked, never immediate: LiquidBlock.onPlace and
//     neighborChanged schedule a tick `getTickDelay` away (water 5, lava
//     30, or 10 in the nether) and the drain runs collect-then-run, so a
//     cascade advances one cell per delay exactly as in vanilla.
//
//   * The lava/water reactions live where MC puts them: LiquidBlock
//     .shouldSpreadLiquid (lava beside water → obsidian for a source,
//     cobblestone otherwise; lava on soul soil beside blue ice → basalt)
//     runs from onPlace/neighborChanged, and LavaFluid.spreadTo (lava
//     falling onto water → stone) runs from the spread itself.
//
// Deliberately NOT ported, and why:
//   * Bubble columns (LiquidBlock's shouldBubbleColumnOccupy branches):
//     there is no BubbleColumnBlock behaviour to schedule.
//   * levelEvent 1501's eight smoke particles: no server→client particle
//     channel exists; the extinguish sound is played through the (logging)
//     sound seam.
//   * ServerLevel.canSpreadFireAround's player-distance gate for the lava
//     random tick: World has no player list. Random ticks already only run
//     in simulating chunks, which is a stricter radius than the rule's
//     default 128 blocks in every configuration this engine ships.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/fluid/FluidState.hpp"

#include <glm/vec3.hpp>
#include <array>

namespace Game {

    class ILevelWrite;
    class JavaRandom;

    namespace Fluids {

        // MC Fluid.getTickDelay: water 5; lava 30, 10 where the dimension is
        // FAST_LAVA (the nether).
        int TickDelay(const ILevelWrite& level, FluidType type);

        // MC Level.scheduleTick(pos, fluid, fluid.getTickDelay(level)). A
        // no-op on a level with no scheduler (the client's prediction) and
        // when the same appointment is already pending.
        void ScheduleTick(ILevelWrite& level, const glm::ivec3& pos, FluidType type);

        // MC FluidState.tick → FlowingFluid.tick. `blockState` is the cell's
        // current block, `fluidState` its fluid (the caller has both).
        void Tick(ILevelWrite& level, const glm::ivec3& pos, BlockState blockState,
                  const FluidState& fluidState);

        // MC LavaFluid.randomTick — fire creeping out of a lava pool.
        void LavaRandomTick(ILevelWrite& level, const glm::ivec3& pos, JavaRandom& random);

        // ── LiquidBlockContainer (SimpleWaterloggedBlock's half) ─────────
        //
        // MC LiquidBlockContainer.canPlaceLiquid(user, level, pos, state,
        // type): a SimpleWaterloggedBlock accepts exactly Fluids.WATER — the
        // SOURCE fluid, not FLOWING_WATER — which is why flowing water runs
        // over a fence without waterlogging it while a bucket or an infinite
        // source next to it does. Kelp, seagrass and the bubble column are
        // containers that refuse everything (they already hold water).
        bool IsLiquidBlockContainer(BlockID id);
        bool CanPlaceLiquid(BlockState state, const FluidState& fluid);
        // MC SimpleWaterloggedBlock.placeLiquid: set WATERLOGGED and book the
        // water tick. Returns whether anything was placed.
        bool PlaceLiquid(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                         const FluidState& fluid);

        // MC LiquidBlock.fizz → levelEvent(1501): the lava-extinguish hiss.
        void Fizz(ILevelWrite& level, const glm::ivec3& pos);

        // MC BaseFireBlock.getState(level, pos): soul fire over a soul-fire
        // base block, plain fire otherwise. Shared with flint and steel.
        BlockState FireStateFor(const IBlockAccess& level, const glm::ivec3& pos);

    } // namespace Fluids

    // Installs LiquidBlock's hooks on BlockID::Water and BlockID::Lava.
    // Called from BlockRegistry::Init after the state tables exist (the
    // hooks read `level`) and before the random-tick table is published
    // (lava random-ticks).
    void BlockRegistry_RegisterFluids(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
