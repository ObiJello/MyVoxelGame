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

} // namespace Game
