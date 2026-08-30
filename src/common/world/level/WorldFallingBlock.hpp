// File: src/common/world/level/WorldFallingBlock.hpp
//
// The one call block code makes into the entity system: "this block has lost
// its support — turn it into a falling entity".
//
// Same shape and same reason as WorldDrops.hpp next door. FallingBlock.cpp
// lives in `common` and must not include server headers, but spawning is
// server authority (the mob managers, the entity tracker and the id space all
// live there). A free function declared here and implemented against
// Server::g_integratedServer keeps the dependency pointing the right way.
//
// Returns null when there is no server to spawn into — a client-side call or a
// torn-down world. The caller has ALREADY cleared the source cell by then,
// exactly as MC's FallingBlockEntity.fall does; vanilla has no failure path
// here either, because on the client the whole tick is gated behind
// `!level.isClientSide()`.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

namespace Game {

    class FallingBlockEntity;
    class ILevelWrite;

    // MC FallingBlockEntity.fall(level, pos, state), minus the block write —
    // the caller does that, because only it knows what the cell should become
    // (air, or the water the block was logged into).
    //
    // `state` is the state the entity CARRIES, already stripped of
    // `waterlogged`. The entity is positioned at the cell's horizontal centre
    // with zero velocity.
    //
    // `level` is needed to find which DIMENSION to spawn into: a block tick
    // in the Nether must not drop its sand into the Overworld. The returned
    // pointer is owned by that level's mob manager and stays valid for the
    // rest of the tick, which is long enough for the caller to arm fall damage
    // (AnvilBlock.falling) or cancel the drop (BrushableBlock.tick).
    FallingBlockEntity* SpawnFallingBlock(ILevelWrite& level,
                                          const glm::ivec3& blockPos,
                                          BlockState state);

} // namespace Game
