// File: src/common/world/level/WorldDrops.hpp
//
// MC's Block.popResource, reachable from common code.
//
// This used to be a stopgap: with no entity system, a "dropped" item was handed
// straight to the nearest player, and callers that could not tolerate failure
// had to check a bool and hold onto the stack. Now that item entities exist it
// does the real thing — spawns one at the block — so the delivery cannot fail
// for want of a nearby player or a free inventory slot.
//
// It stays a free function in `common` (rather than callers reaching for
// Server::ItemEntityManager directly) because block entities and container
// menus live in common and must not depend on server headers.
#pragma once

#include <glm/glm.hpp>

namespace Game {

    struct ItemStack;
    class  ILevelWrite;

    // Spawn `stack` as a dropped item at `pos`, with MC's popResource scatter
    // and hop. Returns false only when there is no server to spawn into (the
    // stack is then still the caller's), which in practice means a client-side
    // call or a torn-down world. An empty stack is a no-op and returns true.
    bool DropItemStackNear(const glm::ivec3& pos, const ItemStack& stack);

    // MC Entity.spawnAtLocation — a drop from an ENTITY, at its exact double
    // position, with no block-centre scatter. See ItemEntityManager::
    // SpawnAtLocation for why the two are not the same call.
    bool DropItemStackAt(const glm::dvec3& pos, const ItemStack& stack);

    // MC Block.popResourceFromFace — same, but nudged out of one face of the
    // block and launched away from it, for items that logically come off a
    // particular side. `face` is a Direction ordinal (0=down .. 5=east).
    bool DropItemStackFromFace(const glm::ivec3& pos, int face, const ItemStack& stack);

    // MC Level.destroyBlock(pos, dropResources=true) — roll the block's loot
    // table, pop the results, then clear the cell with a full update.
    //
    // Shared because three callers now need exactly this and each had (or
    // would have grown) its own copy: the neighbour-update walk in
    // World::NotifyNeighborBlocks, scaffolding losing its last support, and a
    // stalagmite losing its floor.
    //
    // The loot RNG is seeded from the POSITION, not from a shared stream, so a
    // collapse is reproducible and two blocks breaking in the same tick do not
    // draw from each other's rolls.
    void DestroyBlockWithDrops(ILevelWrite& level, const glm::ivec3& pos);

} // namespace Game
