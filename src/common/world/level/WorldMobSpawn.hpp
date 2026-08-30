// File: src/common/world/level/WorldMobSpawn.hpp
//
// MC EntityType.spawn(ServerLevel, …, SPAWN_ITEM_USE), reachable from common.
//
// Same shape and same reason as WorldDrops next door: spawn eggs are ordinary
// item behaviours and item behaviours live in `common`, but the mob managers
// live in `server` and common must not include server headers. One free
// function bridges it, and the include lives in the .cpp.
#pragma once

#include "common/entity/EntityType.hpp"

#include <glm/glm.hpp>

namespace Game {

    // Create a mob of `type` at `spawnPos` and register it with the server.
    //
    // `tryMoveDown` / `movedUp` are MC's EntityType.create arguments and are
    // what stop an egg used on the top of a block from burying the mob in it:
    // the entity is placed one block ABOVE spawnPos and then slid back down
    // until it rests on whatever is actually there. `movedUp` widens that
    // search by one block, for the case where the click already pushed the
    // spawn position up a face.
    //
    // Returns false when there is no server to spawn into, when the type is
    // unknown, or when MC's peaceful-difficulty rule rejects it — MC's own
    // `spawn(...) != null` test, which is what gates consuming the egg.
    bool SpawnMobFromItem(EntityTypeId type, const glm::ivec3& spawnPos,
                          bool tryMoveDown, bool movedUp);

    struct ItemStack;

    // MC EnderEyeItem.use's tail (EnderEyeItem.java:85-101): locate the
    // nearest stronghold, spawn an EyeOfEnder at `from`, and point it there.
    //
    // Its own bridge rather than a call to SpawnMobFromItem because it needs
    // two things only the server can do — the stronghold lookup and
    // `EyeOfEnder::SignalTo` on the spawned entity — and doing them from the
    // item behaviour would mean common reaching into server twice.
    //
    // Returns false when there is no server, or when this world generates no
    // strongholds. MC's own `nearestMapFeature == null` branch returns CONSUME
    // WITHOUT taking the item, which is what stops an eye being wasted in a
    // world that has nowhere to send it — so the caller must not shrink the
    // stack when this answers false.
    bool ThrowEnderEye(int dimensionId, const glm::dvec3& from, const ItemStack& stack);

} // namespace Game
