// File: src/common/world/block/PlantBlocks.hpp
//
// Survival, growth and bone meal for the small plants that stack or float:
//
//   * SeaPickleBlock (26.3) — 1..4 pickles in one cell (the placement half is
//     BlockPlacement's StackedPlacementState), glowing only while waterlogged
//     (the light engine reads that off the state), dead out of water, held up
//     by a block whose collision reaches the top of its cell, and bone meal
//     on coral spreading more pickles over the coral around it;
//   * LilyPadBlock / FrogspawnBlock — sit on a water source (or ice, for the
//     pad) with no fluid in their own cell; PlaceOnWaterBlockItem's clip is
//     the client's (ClientPlayerController) and the server's placement gate;
//   * MushroomBlock — needs a dark cell on a solid block unless the ground is
//     mycelium, podzol or nylium; spreads on its random tick; bone meal grows
//     the huge mushroom (HugeRedMushroomFeature / HugeBrownMushroomFeature,
//     ported against the game's level here — the terrain library's copy only
//     writes into a generating chunk).
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    class ILevelWrite;
    class JavaRandom;

    namespace SeaPickle {

        // 1..4 on a sea pickle, 0 on anything else.
        int Pickles(BlockState state);

        // MC SeaPickleBlock.canSurvive → mayPlaceOn(below): the collision
        // shape of the block below reaches the top of its cell, or its top
        // face is sturdy.
        bool CanSurvive(const IBlockAccess& level, const glm::ivec3& pos);

    } // namespace SeaPickle

    // MC LilyPadBlock.canSurvive → mayPlaceOn(below): a water source below
    // (#supports_lily_pad fluids) or ice / frosted ice (#supports_lily_pad
    // blocks), and no fluid in the pad's own cell.
    bool LilyPadCanSurvive(const IBlockAccess& level, const glm::ivec3& pos);

    // MC FrogspawnBlock.canSurvive → mayPlaceOn(below): a water source below
    // (#supports_frogspawn fluids) and no fluid in its own cell.
    bool FrogspawnCanSurvive(const IBlockAccess& level, const glm::ivec3& pos);

    // MC MushroomBlock.canSurvive: mycelium / podzol / nylium below
    // (#overrides_mushroom_light_requirement), or raw light below 13 at the
    // mushroom's cell and a solid-rendering block below.
    bool MushroomCanSurvive(const IBlockAccess& level, const glm::ivec3& pos);

    // brown_mushroom / red_mushroom.
    bool IsSmallMushroom(BlockID id);

    // MC MushroomBlock.growMushroom: the small mushroom at `pos` becomes the
    // huge one if its feature fits, and stays put otherwise. Returns whether
    // it grew. Server only.
    bool GrowHugeMushroom(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                          JavaRandom& random);

    // Wires the hooks above. Called from BlockRegistry_RegisterBehaviors.
    void RegisterPlantBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
