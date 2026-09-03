// File: src/server/entity/MobManager.hpp
//
// Owns every mob in the world and drives it, mirroring the shape of
// Server::ItemEntityManager (see that header for the reasoning behind putting
// this under src/server/ rather than on Game::World).
//
// One structural difference from ItemEntityManager, and it is the point of the
// class: mobs are ticked in TWO phases per server tick —
//
//   1. CheckDespawn, for mobs inside ENTITY-ticking range only. MC does the
//      same and it is easy to get wrong: ServerLevel.java:371 iterates
//      `entityTickList`, which onTickingStart/onTickingEnd (:1810/:1815) keep
//      to exactly the entity-ticking set, and checkDespawn sits inside that
//      forEach at :375. This header previously claimed the opposite — that MC
//      despawn-checks every loaded mob — and the code followed the comment,
//      which cost a GetNearestPlayer call per loaded entity per tick.
//   2. Tick, for the same set, after the despawn check.
//
// The spatial index is rebuilt once per tick rather than maintained
// incrementally. Goals query it constantly (every target scan, every breeding
// check, every alert) but mobs move at most ~0.3 blocks per tick, so a rebuild
// is both simpler and cheaper than keeping buckets correct on every move.
//
// Note the argument cuts the other way at scale: because mobs move slowly,
// almost none change chunk, which is exactly what makes incremental
// maintenance nearly free and a full rebuild pure waste. MC maintains it
// incrementally from Entity.setPosRaw (PersistentEntitySectionManager
// Callback.onMove). The rebuild now at least reuses its bucket vectors rather
// than reallocating them; going incremental needs a setter chokepoint on
// Entity::position, which is currently a public raw field written at ~141
// sites.
#pragma once

#include "common/entity/Entity.hpp"
#include "common/entity/EntityIdAllocator.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/EntityType.hpp"
#include "common/world/math/WorldMath.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game { class Mob; class World; struct AABB; class JavaRandom; }

namespace Server {

    class ChunkTicketManager;

    class ServerLevelBridge;

    class MobManager {
    public:
        explicit MobManager(ServerLevelBridge* level);
        ~MobManager();

        // Takes ownership and assigns an id. Returns the id, or 0 on failure.
        int32_t Add(std::unique_ptr<Game::Mob> mob);

        // ── Moving a mob between levels (immersive portal travel) ────────
        // Take a live mob out of this manager WITHOUT destroying it. Every
        // other mob's and player view's reference to it is cleared first,
        // exactly as for a death — it is leaving this level's simulation.
        // Null if the id is unknown.
        std::unique_ptr<Game::Mob> Extract(int32_t id);
        // The other half: insert a mob that KEEPS its id and uuid (ids are
        // process-wide, see EntityIdAllocator). False if the id is taken.
        bool AddExisting(std::unique_ptr<Game::Mob> mob);

        // One server tick. `tickingChunks` is the block-ticking set — mobs
        // outside it are despawn-checked but not simulated.
        //
        // `outRemoved` receives the ids of mobs that died or despawned this
        // tick, for the removal broadcast.
        // MC ServerLevel.tick's entityTickList lambda gates each entity on
        // distanceManager.inEntityTickingRange(entity.chunkPosition()), re-read
        // LIVE from the entity's own position every tick. It does not consult a
        // precomputed set — a set is a snapshot, and a snapshot of "where the
        // player is" is exactly the thing that went stale and froze the world.
        //
        // `tickets` may be null (tests, a level with no ticket manager), in
        // which case everything ticks — failing open, not closed.
        void Tick(const ChunkTicketManager* tickets,
                  std::vector<int32_t>& outRemoved);

        // Every mob whose box overlaps `box`, excluding `except`.
        void CollectInBox(const Game::AABB& box, const Game::Entity* except,
                          std::vector<Game::Entity*>& out) const;

        // Drop every mob in a chunk that is being unloaded. Mobs are memory-
        // only (there is no entity NBT layer — see the plan's scope note), so
        // this is where they cease to exist.
        void RemoveInChunk(Game::Math::ChunkPos chunk, std::vector<int32_t>& outRemoved);
        // The same for MANY chunks at once, in one pass over the mobs. The
        // unload sweep hands over hundreds of chunks in one call, and the
        // per-chunk form walked every mob several times per chunk — 653
        // chunks against 1.37M falling blocks was an 11.5-second stall.
        void RemoveInChunks(const std::vector<Game::Math::ChunkPos>& chunks,
                            std::vector<int32_t>& outRemoved);

        void Clear();

        const std::unordered_map<int32_t, std::unique_ptr<Game::Mob>>& All() const {
            return m_mobs;
        }
        size_t Count() const { return m_mobs.size(); }
        // The live mobs in tick (insertion) order — the walk every per-mob
        // pass outside Tick should take instead of All(): a node map walk is
        // a cache miss per mob, the vector is sequential. Invalid only while
        // Tick itself is running.
        const std::vector<Game::Mob*>& List() const { return m_mobList; }

        // CollectInBox across the worker pool: bucket lists are gathered
        // serially, the AABB tests run in parallel per bucket, and the
        // results are concatenated in bucket order. For a mass-detonation
        // query that touches a million entities the serial form was tens of
        // milliseconds per tick.
        void CollectInBoxParallel(const Game::AABB& box, const Game::Entity* except,
                                  std::vector<Game::Entity*>& out) const;

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

        // Mint an entity id from the mob range without adding a mob — for
        // the compact falling-block store, whose entries share this range
        // (the client tells mobs from items and players by id alone) and
        // must never collide with a real mob's id.
        // Process-wide, so ids never collide across dimensions (EntityIdAllocator.hpp).
        int32_t AllocateId() { return Game::AllocateMobEntityId(); }

        // Is an entity with this identity already live in this level?
        bool HasUuid(const Game::Uuid& uuid) const { return m_byUuid.count(uuid) != 0; }

        // Identity lookup, for resolving a saved cross-entity reference.
        Game::Mob* FindByUuid(const Game::Uuid& uuid) const {
            const auto it = m_byUuid.find(uuid);
            return it == m_byUuid.end() ? nullptr : Find(it->second);
        }

    private:
        void RebuildIndex();

        // MC ServerLevel.tickPassenger: after a vehicle ticks, its riders get
        // rideTick (vehicle-driven positioning) instead of a normal tick, then
        // THEIR riders recursively. A rider whose link went stale mid-tick is
        // dismounted, exactly as MC's else-branch does.
        void TickPassengerChain(Game::Entity& vehicle);

        // Primed TNT deferred out of the serial tick loop and run across the
        // worker pool. A member so the storage survives across ticks — a
        // detonation refills this every tick and would otherwise allocate a
        // hundred thousand pointers' worth each time.
        std::vector<Game::Mob*> m_parallelTickBatch;
        // Falling blocks deferred out of the serial loop, IN LIST ORDER (an
        // ordered collection, not the atomic-cursor batch TNT uses — the
        // landing half must run in insertion order). Members for capacity.
        std::vector<Game::FallingBlockEntity*>                 m_fallingBatch;
        std::vector<Game::FallingBlockEntity::PhysicsSnapshot> m_fallingSnapshots;
        std::vector<uint8_t>                                   m_fallingOutcome;
        // Cells the landing half wrote this tick, keyed by (x,z) column with
        // the y range: the conflict test for the retry rule.
        std::unordered_map<uint64_t, std::pair<int, int>>      m_writtenColumns;

        // THE LIVE MOB LIST, in insertion order — the order the tick loop
        // runs, exactly MC's entityTickList (an insertion-ordered linked hash
        // map). Maintained by Add and the two sweeps (order-preserving
        // compaction, never swap-and-pop); RebuildIndex derives the spatial
        // buckets from it. It is NOT rebuilt from m_mobs: that map is
        // unordered, and ticking in hash order broke falling-sand columns
        // (see RebuildIndex).
        std::vector<Game::Mob*> m_mobList;
        // Per-tick classification, parallel to m_mobList.
        std::vector<uint8_t>    m_mobClass;
        std::vector<uint16_t>   m_mobTypeIdx;
        // Per-tick end-of-tick scan, parallel to m_mobList: bit 0 removed,
        // bit 1 dead-or-dying. Filled across the pool so the death-loot pass,
        // the dying list and the sweep's compaction never walk the entities.
        std::vector<uint8_t>    m_mobFlags;
        // Chunk keys, parallel to m_mobList, computed across the pool for
        // RebuildIndex.
        std::vector<uint64_t>   m_mobKeys;

        // MC LivingEntity.dropAllDeathLoot (LivingEntity.java:1478-1485) —
        // the generated per-type loot tables, the sheep's colour-dependent
        // wool (which no static row can express; see gen_mob_loot.py), the
        // per-mob custom drops, and the XP drop when the kill is credited to
        // a player. Fired ONCE per mob, at death (deathTime 0), not at the
        // removal sweep 20 ticks later — see the pass in Tick().
        void DropDeathLoot(Game::Mob& mob);

        // MC LootTable.getRandomItems over the baked pool structure: per
        // pool, check conditions, roll N times, ONE weighted entry per roll.
        void EvaluateLootTable(Game::Mob& mob, bool killedByPlayer,
                               Game::JavaRandom& rng);

        ServerLevelBridge* m_level;
        std::unordered_map<int32_t, std::unique_ptr<Game::Mob>> m_mobs;

        // UUID -> entity id, maintained wherever m_mobs is.
        //
        // This is the duplication guard for entity loading: a chunk can be
        // asked to load while its entities are already live (a re-entry
        // during an in-flight read, or two code paths both making a chunk
        // live). Without it a double-load is a herd of cloned cows; with it
        // the loader simply skips anything already present.
        std::unordered_map<Game::Uuid, int32_t, Game::UuidHash> m_byUuid;

        // Ids are handed out from the mob range so the client can tell a mob
        // removal from a player or item removal by id alone.

        // chunk key -> mob ids. Rebuilt each tick; see the header note.
        std::unordered_map<uint64_t, std::vector<Game::Mob*>> m_byChunk;

        // Ids whose death loot already dropped — the once-only latch for the
        // death-drop pass (a corpse lies around for 20 ticks after dropping).
        // Entries leave with their mob in the sweeps.
        std::unordered_set<int32_t> m_deathLootDropped;

        std::atomic<int> m_categoryCounts[8] = {};
        std::atomic<int> m_typeCounts[static_cast<size_t>(Game::EntityTypeId::Count)] = {};
        std::atomic<int> m_spawnableChunks{0};
    };

} // namespace Server
