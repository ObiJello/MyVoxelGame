// File: src/common/world/block/CandleBlocks.hpp
//
// Mirrors net.minecraft.world.level.block.AbstractCandleBlock, CandleBlock,
// CandleCakeBlock and CakeBlock (26.3):
//
//   * the 17 candles stack 1..4 in one cell (CandleBlock.canBeReplaced /
//     getStateForPlacement — the placement half lives in BlockPlacement's
//     StackedPlacementState), light 3 per candle while LIT (the light engine
//     reads that off the state, GeneratedBlockLight.inc), waterlog, and go out
//     on an empty-hand click, in water, in an explosion or under a splash of
//     water;
//   * a candle clicked onto an uneaten cake becomes that colour's candle cake
//     (CakeBlock.useItemOn); eating a slice of one drops the candle and leaves
//     a cake with one bite taken (CandleCakeBlock.useWithoutItem);
//   * flint and steel, a fire charge and a burning projectile light either
//     (CandleBlock.canLight / CandleCakeBlock.canLight, onProjectileHit).
//
// The item side (flint and steel, fire charge) is in ItemBehaviors; the
// waterlogging extinguish is CandlePlaceLiquid below, called from
// Fluids::PlaceLiquid.
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    class ILevelWrite;
    class Entity;
    class IUsePlayer;

    namespace Candles {

        // #minecraft:candles — the plain candle and the sixteen dyed ones.
        bool IsCandle(BlockID id);
        // #minecraft:candle_cakes.
        bool IsCandleCake(BlockID id);
        // MC CandleCakeBlock.byCandle: the cake that carries this candle, or
        // Air for anything that is not a candle.
        BlockID CandleCakeOf(BlockID candle);

        // MC CandleBlock.canLight: an unlit, dry candle.
        bool CandleCanLight(BlockState state);
        // MC CandleCakeBlock.canLight: an unlit candle cake.
        bool CandleCakeCanLight(BlockState state);
        // Either — the test FlintAndSteelItem / FireChargeItem open with.
        inline bool CanLight(BlockState state) {
            return CandleCanLight(state) || CandleCakeCanLight(state);
        }

        // MC AbstractCandleBlock.isLit: a candle or candle cake with LIT set.
        bool IsLit(BlockState state);

        // 1..4 on a candle, 0 on anything else.
        int CandleCount(BlockState state);

        // `state.setValue(LIT, lit)`.
        BlockState WithLit(BlockState state, bool lit);

        // MC AbstractCandleBlock.extinguish(player, state, level, pos):
        // LIT off (flags 11), a SMOKE puff from every wick (client-side, so
        // the acting player's prediction shows it), CANDLE_EXTINGUISH for
        // everyone. `state` is the state to write with LIT cleared — the
        // waterlogging path passes the already-waterlogged one.
        void Extinguish(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                        Entity* player = nullptr);

        // MC CandleBlock.canSurvive: Block.canSupportCenter(level, below, UP)
        // — the top of the block below covers the 2x2-pixel centre.
        bool CandleCanSurvive(const IBlockAccess& level, const glm::ivec3& pos);

        // MC CakeBlock.canSurvive / CandleCakeBlock.canSurvive:
        // `level.getBlockState(pos.below()).isSolid()`.
        bool CakeCanSurvive(const IBlockAccess& level, const glm::ivec3& pos);

        // MC CandleBlock.placeLiquid: water filling a candle waterlogs it and,
        // when it was lit, puts it out (extinguish, not a plain write).
        // Returns false for anything that is not a dry candle taking a water
        // source — the caller then runs the generic SimpleWaterloggedBlock
        // path. Writes nothing on the client, like the generic path.
        bool CandlePlaceLiquid(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

    } // namespace Candles

    // Wires the candle, candle cake and cake hooks. Called from
    // BlockRegistry_RegisterBehaviors.
    void RegisterCandleBehaviors(std::array<Block, BlockRegistry::Size>& blocks);
    // MC ComposterBlock (ComposterBlock.cpp).
    void RegisterComposterBehaviors(std::array<Block, BlockRegistry::Size>& blocks);
    // MC BeehiveBlock (BeehiveBlock.cpp): honey harvest, comparator, fire.
    void RegisterBeehiveBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

    // MC ShearsDispenseItemBehavior.tryShearBeehive: a beehive or bee nest
    // at `pos` full of honey (level 5) is sheared — BEEHIVE_SHEAR, the
    // honeycomb dropped, the bees let out (BEE_RELEASED) and the honey reset,
    // the SHEAR game event. False (nothing done) otherwise. Server side.
    bool TryShearBeehiveFromDispenser(ILevelWrite& level, const glm::ivec3& pos);

} // namespace Game
