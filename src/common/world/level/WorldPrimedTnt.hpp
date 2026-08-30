// File: src/common/world/level/WorldPrimedTnt.hpp
//
// TNT's half of the same seam WorldFallingBlock.hpp opens: block code in
// `common` needs to spawn an entity, and spawning is server authority.
//
// Returns false when there is no server, or when the `tnt_explodes` gamerule
// is off — the caller reads that second case to leave the block alone and tell
// the player, rather than silently eating a durability point.
#pragma once

#include <glm/glm.hpp>

namespace Game {

    class Entity;
    class ILevelWrite;

    // MC TntBlock.prime's entity half: a PrimedTnt at the cell's horizontal
    // centre with MC's random spawn hop and an 80-tick fuse.
    //
    // Does NOT clear the cell; MC's callers each remove the block themselves
    // and they do it with different update flags.
    bool SpawnPrimedTnt(ILevelWrite& level, const glm::ivec3& blockPos, Entity* igniter);

} // namespace Game
