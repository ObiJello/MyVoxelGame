// File: src/common/entity/EntityIdAllocator.hpp
//
// Process-wide entity id allocation — MC's `Entity.ENTITY_COUNTER`.
//
// Each ServerLevel owns its own mob, item and orb managers, and each used to
// count ids from the same per-type base — so a Nether zombie and an Overworld
// cow could share an id. That was tolerable while a client only ever held one
// dimension and every removal broadcast was dimension-scoped. It is not once a
// client holds two dimensions at once (immersive portals): the client keys
// entities by id, so the ids must be unique across the whole server.
//
// The per-type RANGES are kept (Entity.hpp) because the client still tells
// players, items, mobs and orbs apart by range. Within a range the counter
// wraps, as MC's does at 2^31; a range is 16 million ids, which at a thousand
// spawns a second is four and a half hours before the first reuse — and a
// reused id is only a problem if its previous owner is still alive somewhere.
#pragma once

#include "common/entity/Entity.hpp"
#include "common/entity/ItemEntity.hpp"

#include <atomic>
#include <cstdint>

namespace Game {

    inline std::atomic<uint32_t> g_mobEntityIdCounter{0};
    inline std::atomic<uint32_t> g_itemEntityIdCounter{0};
    inline std::atomic<uint32_t> g_xpOrbEntityIdCounter{0};

    inline int32_t AllocateMobEntityId() {
        constexpr uint32_t kRange = static_cast<uint32_t>(kXpOrbEntityIdBase - kMobEntityIdBase);
        return kMobEntityIdBase + static_cast<int32_t>(
            g_mobEntityIdCounter.fetch_add(1, std::memory_order_relaxed) % kRange);
    }

    inline int32_t AllocateItemEntityId() {
        constexpr uint32_t kRange = static_cast<uint32_t>(kMobEntityIdBase - kItemEntityIdBase);
        return kItemEntityIdBase + static_cast<int32_t>(
            g_itemEntityIdCounter.fetch_add(1, std::memory_order_relaxed) % kRange);
    }

    inline int32_t AllocateXpOrbEntityId() {
        constexpr uint32_t kRange = static_cast<uint32_t>(0x7FFF'FFFF - kXpOrbEntityIdBase);
        return kXpOrbEntityIdBase + static_cast<int32_t>(
            g_xpOrbEntityIdCounter.fetch_add(1, std::memory_order_relaxed) % kRange);
    }

} // namespace Game
