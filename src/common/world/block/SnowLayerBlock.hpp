// File: src/common/world/block/SnowLayerBlock.hpp
//
// Mirrors net.minecraft.world.level.block.SnowLayerBlock (26.3) — `minecraft:
// snow`, BlockID::SnowLayer, the 1-8 `layers` block — and SnowyBlock, the
// `snowy` flag grass, mycelium and podzol raise when anything in
// #minecraft:snow sits on them.
//
// What lives where:
//   * the outline / support / visual shape is SHAPES[layers], which is exactly
//     the snow_height2 … snow_height14 / snow_block model the blockstate maps
//     each layer count to, so BlockRegistry's model-derived shape already is
//     it (and the mesher culls from it: a full 8-layer pile hides its
//     neighbours' faces, a partial one only the face under it);
//   * the COLLISION shape is SHAPES[layers - 1] — one layer lower, and empty
//     for a single layer — served from CollisionBox below through
//     BlockRegistry::GetBlockCollisionShapeSet / GetSingleCollisionBox;
//   * canSurvive, updateShape, the block-light melt and the placement growth
//     are here, hooked up by RegisterSnowBehaviors.
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    class ILevelWrite;

    namespace SnowLayer {

        // MC SnowLayerBlock.MAX_HEIGHT / HEIGHT_IMPASSABLE.
        constexpr int kMaxHeight        = 8;
        constexpr int kHeightImpassable = 5;

        // MC state.getValue(LAYERS) — 1..8 for a snow layer, 0 for anything
        // else.
        int Layers(BlockState state);

        // #minecraft:snow (data/minecraft/tags/block/snow.json): the snow
        // layer, snow_block and powder_snow — SnowyBlock.isSnowySetting.
        bool IsInSnowTag(BlockID id);

        // MC SnowLayerBlock.canSurvive, of the block below `pos`:
        //   #cannot_support_snow_layer (ice, packed_ice, barrier) -> false;
        //   #support_override_snow_layer (honey_block, soul_sand, mud) -> true;
        //   else its COLLISION shape's top face is full, or it is a
        //   full 8-layer snow pile.
        bool CanSurvive(const IBlockAccess& level, const glm::ivec3& pos);

        // MC SnowLayerBlock.getStateForPlacement against a snow layer already
        // in the resolved cell: one more layer, at most eight. Returns
        // `existing` unchanged when it is not a snow layer or already full.
        BlockState GrownState(BlockState existing);

        // MC getCollisionShape: SHAPES[layers - 1], a full-footprint box
        // (layers - 1) * 2 pixels tall. Null for a single layer, whose
        // collision shape is empty (you sink the 2 px into it), and for any
        // state that is not a snow layer.
        const BlockRegistry::BlockShape* CollisionBox(BlockState state);

        // MC SnowLayerBlock.isPathfindable(LAND): fewer than
        // HEIGHT_IMPASSABLE layers. Every other computation type is false.
        bool IsPathfindableLand(BlockState state);

    } // namespace SnowLayer

    // The SnowyBlock family — the blocks carrying the `snowy` property
    // (grass_block, mycelium, podzol, and the Aether's grass, whose
    // AetherGrassBlock extends the same class chain).
    bool IsSnowyBlock(BlockID id);

    // MC SnowyBlock.getStateForPlacement: `snowy` from whether the block
    // above is in #minecraft:snow. Returns `state` unchanged for a block
    // outside the family.
    BlockState SnowyPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState state);

    // Wires SnowLayerBlock's updateShape / randomTick and SnowyBlock's
    // updateShape. Called from BlockRegistry::Init after InitBlockStates
    // (every hook reads a property) and before the random-tick table is
    // published (the snow layer random-ticks).
    void BlockRegistry_RegisterSnow(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
