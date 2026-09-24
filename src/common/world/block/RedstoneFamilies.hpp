// File: src/common/world/block/RedstoneFamilies.hpp
//
// Membership tests for the redstone-adjacent block families, plus the
// per-family constants MC keeps on BlockSetType (how long a button stays
// pressed, whether a plate feels items). All answered from the registry slug
// and cached per BlockID on first use, so a wood type added to
// BlockDefs.inc joins its family without a code change.
#pragma once

#include "Blocks.hpp"

namespace Game {

    // Every *_button. `IsWoodenButton` excludes stone and polished blackstone —
    // MC's BlockSetType.STONE / POLISHED_BLACKSTONE, which press for 20 ticks
    // and cannot be shot with an arrow, against wood's 30 ticks and yes.
    bool IsButtonBlock(BlockID id);
    bool IsWoodenButton(BlockID id);
    int  ButtonTicksToStayPressed(BlockID id);

    // Every *_pressure_plate, including the two weighted ones.
    bool IsPressurePlateBlock(BlockID id);
    // light_weighted / heavy_weighted — the POWER-valued plates.
    bool IsWeightedPressurePlate(BlockID id);
    // MC WeightedPressurePlateBlock's maxWeight: 15 for gold, 150 for iron.
    int  WeightedPlateMaxWeight(BlockID id);
    // BlockSetType.pressurePlateSensitivity == MOBS (stone, blackstone): only
    // living entities press it. Wood feels everything, items included.
    bool PressurePlateMobsOnly(BlockID id);

    // rail, powered_rail, detector_rail, activator_rail — MC BaseRailBlock.
    bool IsRailBlock(BlockID id);
    // Everything but the plain rail: MC BaseRailBlock.isStraight, the rails
    // that cannot form corners.
    bool IsStraightRail(BlockID id);

    // repeater or comparator — MC DiodeBlock.isDiode.
    bool IsDiodeBlock(BlockID id);
    // Anything that produces, carries, delays or reacts to a signal by its
    // own tick: what makes a chunk worth keeping loaded for redstone's sake
    // (ChunkKeeper's redstone index).
    bool IsRedstoneComponent(BlockID id);

    // torch / soul_torch / redstone_torch and their wall twins, for the
    // StandingAndWallBlockItem placement swap.
    bool IsStandingTorch(BlockID id);
    bool IsWallTorch(BlockID id);
    // The wall block a standing torch places as (Air when it has none).
    BlockID WallTorchOf(BlockID standing);

    // MC BlockSetType.canOpenByHand is false for iron only (doors, trapdoors).
    bool IsIronDoorLike(BlockID id);

    void InitRedstoneFamilies();

} // namespace Game
