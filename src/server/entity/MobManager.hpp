// File: src/server/entity/MobManager.hpp
//
// Owns every mob in the world and drives it, mirroring the shape of
// Server::ItemEntityManager (see that header for the reasoning behind putting
// this under src/server/ rather than on Game::World).
//
// One structural difference from ItemEntityManager, and it is the point of the
// class: mobs are ticked in TWO phases per server tick —
//
//   1. CheckDespawn for EVERY mob, whether or not it is in ticking range. MC
//      does the same, and it is what stops a world from accumulating mobs in
//      chunks that are loaded but not simulated.
//   2. Tick only for mobs inside the block-ticking set.
//
// The spatial index is rebuilt once per tick rather than maintained
// incrementally. Goals query it constantly (every target scan, every breeding
// check, every alert) but mobs move at most ~0.3 blocks per tick, so a rebuild
// is both simpler and cheaper than keeping buckets correct on every move.
#pragma once

#include "common/entity/Entity.hpp"
#include "common/entity/EntityType.hpp"
#include "common/world/math/WorldMath.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game { class Mob; class World; struct AABB; }

namespace Server {

    class ServerLevelBridge;

    class MobManager {
    public:
        explicit MobManager(ServerLevelBridge* level);
        ~MobManager();

        // Takes ownership and assigns an id. Returns the id, or 0 on failure.
        int32_t Add(std::unique_ptr<Game::Mob> mob);

        // One server tick. `tickingChunks` is the block-ticking set — mobs
        // outside it are despawn-checked but not simulated.
        //
        // `outRemoved` receives the ids of mobs that died or despawned this
        // tick, for the removal broadcast.
        void Tick(const std::unordered_set<uint64_t>& tickingChunks,
                  std::vector<int32_t>& outRemoved);

        // Every mob whose box overlaps `box`, excluding `except`.
        void CollectInBox(const Game::AABB& box, const Game::Entity* except,
                          std::vector<Game::Entity*>& out) const;

        // Drop every mob in a chunk that is being unloaded. Mobs are memory-
        // only (there is no entity NBT layer — see the plan's scope note), so
        // this is where they cease to exist.
        void RemoveInChunk(Game::Math::ChunkPos chunk, std::vector<int32_t>& outRemoved);

        void Clear();

        const std::unordered_map<int32_t, std::unique_ptr<Game::Mob>>& All() const {
            return m_mobs;
        }
        size_t Count() const { return m_mobs.size(); }

        // Per-category and per-type live counts, recomputed during Tick.
        //
        // ATOMIC because the debug panel reads them from the MAIN thread while
        // the server thread writes them. That panel used to walk All() instead,
        // which is an unsynchronised iteration over a map the server thread
        // inserts into and erases from — a crash, not a stale number. Anything
        // the UI needs about mobs must come through counters like these, or
        // through a snapshot built on the server thread.
        int CountForCategory(int category) const;
        int CountForType(uint16_t type) const;

        // The spawnable chunk count the spawner last used, for the debug
        // panel's cap display. Atomic because the server thread writes it and
        // the render thread reads it; it is a diagnostic, so a torn read would
        // be harmless anyway — the atomic is here to keep it defined.
        void SetSpawnableChunkCount(int n) { m_spawnableChunks.store(n, std::memory_order_relaxed); }
        int  GetSpawnableChunkCount() const { return m_spawnableChunks.load(std::memory_order_relaxed); }

        Game::Mob* Find(int32_t id) const;

    private:
        void RebuildIndex();

        // MC LivingEntity.dropAllDeathLoot — the generated per-type loot
        // tables, plus the sheep's colour-dependent wool (which no static row
        // can express; see the note in gen_mob_loot.py).
        void DropLoot(Game::Mob& mob);

        ServerLevelBridge* m_level;
        std::unordered_map<int32_t, std::unique_ptr<Game::Mob>> m_mobs;

        // Ids are handed out from the mob range so the client can tell a mob
        // removal from a player or item removal by id alone.
        int32_t m_nextId = Game::kMobEntityIdBase;

        // chunk key -> mob ids. Rebuilt each tick; see the header note.
        std::unordered_map<uint64_t, std::vector<Game::Mob*>> m_byChunk;

        std::atomic<int> m_categoryCounts[8] = {};
        std::atomic<int> m_typeCounts[static_cast<size_t>(Game::EntityTypeId::Count)] = {};
        std::atomic<int> m_spawnableChunks{0};
    };

} // namespace Server
