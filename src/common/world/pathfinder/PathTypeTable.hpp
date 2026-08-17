// File: src/common/world/pathfinder/PathTypeTable.hpp
//
// MC WalkNodeEvaluator.getPathTypeFromState, precomputed per BlockID.
//
// MC asks each BlockState a chain of `is(...)` questions every time it
// classifies a position, backed by block tags. This engine has no tag system in
// the block registry, but it does have `Block::registrySlug` — the vanilla
// registry name — which is exactly what those tags are derived from. So the
// whole chain is evaluated ONCE per block id at startup and the answer cached
// in a flat array.
//
// That is not just an optimisation. Pathfinding classifies on the order of a
// thousand positions per search, several searches per second, and doing string
// comparisons in that loop would make navigation the most expensive thing in
// the server tick.
//
// Doors are deliberately classified as CLOSED. Their open/closed state lives in
// a state index this table is not keyed on, and none of the eight mobs can open
// or break doors — so "closed" is the correct answer for all of them, and the
// place to revisit if a door-breaking zombie is ever added.
#pragma once

#include "common/world/pathfinder/PathType.hpp"
#include "common/world/block/Blocks.hpp"

namespace Game {

    // Build the table. Must run after BlockRegistry::Init(), and is re-runnable
    // — BlockRegistry re-registers blocks on world reload.
    void InitPathTypeTable();

    // The block's own path type, ignoring its neighbours. This is MC's
    // getPathTypeFromState; the neighbour scan that turns it into WALKABLE /
    // WATER_BORDER / DANGER_FIRE lives in WalkNodeEvaluator.
    PathType GetPathTypeFromBlock(BlockID id);

} // namespace Game
