// File: src/common/world/block/LegacySolid.hpp
//
// MC BlockStateBase.isSolid() — the "legacy solid" flag (legacySolid, cached
// per state by calculateSolid):
//
//   forceSolidOn  → solid       (signs, banners, walls, lanterns, shulker boxes…)
//   forceSolidOff → not solid   (ladders, snow layers, end rods, azaleas…)
//   otherwise     → the COLLISION shape's bounds: solid when their average
//                   side is at least 0.7291666… (35/48) or they are a full
//                   block tall; an empty collision shape is never solid.
//
// Not "is a full cube" and not "blocks motion": a fence (1.5 tall) is solid,
// a slab (average 0.83) is solid, a carpet is not. What still asks this
// question in MC: what a hanging entity (painting) may hang on
// (HangingEntity.isSupportingBlock), and a handful of placement rules.
//
// The force flags are Properties, copied between blocks, so they are
// generated from Blocks.java (tools/gen_legacy_solid.py →
// GeneratedLegacySolid.inc); the shape half reads the engine's collision
// shapes, per state.
#pragma once

#include "common/world/block/BlockState.hpp"

namespace Game {

    bool IsLegacySolid(BlockState state);

} // namespace Game
