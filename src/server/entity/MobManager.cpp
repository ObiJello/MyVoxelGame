// File: src/server/entity/MobManager.cpp
#include "server/entity/MobManager.hpp"
#include "common/core/Log.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/loot/GeneratedMobLoot.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/core/JavaRandom.hpp"

#include "common/core/TickParallel.hpp"

#include <atomic>

#include <algorithm>
#include <climits>
#include <cmath>

namespace Server {

    namespace {
        uint64_t ChunkKey(int cx, int cz) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                    static_cast<uint32_t>(cz);
        }
        uint64_t ChunkKeyOf(const Game::Entity& e) {
            return ChunkKey(static_cast<int>(std::floor(e.position.x)) >> 4,
                            static_cast<int>(std::floor(e.position.z)) >> 4);
        }
    }

    MobManager::MobManager(ServerLevelBridge* level) : m_level(level) {}
    MobManager::~MobManager() = default;

    int32_t MobManager::Add(std::unique_ptr<Game::Mob> mob) {
        if (!mob) return 0;
        const int32_t id = AllocateId();
        mob->SetId(id);
        // Mint only if unset: the entity loader stamps a saved UUID before
        // handing the mob over, and that identity must survive.
        mob->MintUuidIfUnset();
        Game::Mob* ptr = mob.get();
        m_byUuid[ptr->GetUuid()] = id;
        m_mobs.emplace(id, std::move(mob));
        // The tick order is INSERTION order — see m_mobList's header note.
        m_mobList.push_back(ptr);
        // Into the spatial index NOW, not at the next rebuild: the natural
        // spawner adds pack members one by one, and MC's checkSpawnObstruction
        // for member N must be able to collide with member N-1 placed the same
        // tick. The index is rebuilt every Tick anyway, so a same-tick
        // insertion can only make queries MORE complete.
        ptr->spatialIndexKey = ChunkKeyOf(*ptr);
        m_byChunk[ptr->spatialIndexKey].push_back(ptr);
        return id;
    }

    std::unique_ptr<Game::Mob> MobManager::Extract(int32_t id) {
        auto it = m_mobs.find(id);
        if (it == m_mobs.end()) return nullptr;
        Game::Mob* leaving = it->second.get();

        // Riding links: the mob is not removed, but its vehicle and riders
        // stay in this level, so the links cannot survive the move.
        leaving->EjectPassengers();
        leaving->StopRiding();

        // Announce the departure to everything that might point at it —
        // the same rule as the death sweep (see Entity::HoldsEntityRefs).
        for (Game::Mob* other : m_mobList) {
            if (other != leaving && other->HoldsEntityRefs()) other->ClearReferenceTo(leaving);
        }
        if (m_level) {
            for (PlayerEntityView* view : m_level->PlayerViews()) view->ClearReferenceTo(leaving);
        }

        // Order-preserving erase from the tick list, then the maps.
        m_mobList.erase(std::remove(m_mobList.begin(), m_mobList.end(), leaving), m_mobList.end());
        m_deathLootDropped.erase(id);
        m_byUuid.erase(leaving->GetUuid());
        std::unique_ptr<Game::Mob> owned = std::move(it->second);
        m_mobs.erase(it);
        RebuildIndex();
        return owned;
    }

    bool MobManager::AddExisting(std::unique_ptr<Game::Mob> mob) {
        if (!mob) return false;
        const int32_t id = mob->GetId();
        if (id == 0 || m_mobs.count(id)) return false;
        mob->MintUuidIfUnset();
        Game::Mob* ptr = mob.get();
        m_byUuid[ptr->GetUuid()] = id;
        m_mobs.emplace(id, std::move(mob));
        m_mobList.push_back(ptr);
        ptr->spatialIndexKey = ChunkKeyOf(*ptr);
        m_byChunk[ptr->spatialIndexKey].push_back(ptr);
        return true;
    }

    Game::Mob* MobManager::Find(int32_t id) const {
        const auto it = m_mobs.find(id);
        return it == m_mobs.end() ? nullptr : it->second.get();
    }

    int MobManager::CountForCategory(int category) const {
        if (category < 0 || category >= 8) return 0;
        return m_categoryCounts[category].load(std::memory_order_relaxed);
    }

    int MobManager::CountForType(uint16_t type) const {
        if (type >= static_cast<uint16_t>(Game::EntityTypeId::Count)) return 0;
        return m_typeCounts[type].load(std::memory_order_relaxed);
    }

    void MobManager::RebuildIndex() {
        // Clear each bucket IN PLACE rather than clearing the map, so the
        // vectors keep their capacity across ticks. Identical query results —
        // this only changes where the memory comes from — but it removes a
        // full deallocate/reallocate cycle per occupied chunk per rebuild, and
        // this runs at the top of every tick and again after any erase.
        //
        // Empty buckets are kept. With mobs clustered into a few chunks that is
        // a handful of empty vectors; the alternative is churning the very
        // allocations this exists to avoid.
        PROFILE_ZONE_N("Mob.RebuildIndex");
        for (auto& [key, bucket] : m_byChunk) bucket.clear();
        // Walked from the ORDERED list, which is maintained by Add and the
        // sweeps rather than rebuilt here. This used to rebuild m_mobList
        // from m_mobs — an unordered_map — which made the tick order hash
        // order, and that is not a cosmetic detail: MC's entityTickList is
        // insertion-ordered, and a column of falling sand depends on it.
        // The block below lands (and becomes a block) BEFORE the one above
        // it moves, so the upper one collides with the fresh block and stops
        // a cell higher. Ticked in hash order the upper one often moved
        // first, sank into the cell the lower one was about to fill, "landed"
        // there (the collider ignores a box it already overlaps and clips it
        // on the block beneath), found the cell taken and popped as an item.
        // A 176k-block sand pyramid shed tens of thousands of items that way.
        //
        // Keys across the pool (the one write is the mob's own
        // spatialIndexKey), then the bucket pushes serially with the last
        // bucket cached: insertion order puts neighbouring mobs in the same
        // chunk far more often than not, so most pushes skip the hash
        // lookup. At a hundred thousand entities the plain walk was 4.6 ms a
        // tick, most of it the cache miss per entity.
        const size_t n = m_mobList.size();
        m_mobKeys.resize(n);
        const auto keyOf = [this](size_t i) {
            Game::Mob* mob = m_mobList[i];
            mob->spatialIndexKey = ChunkKeyOf(*mob);
            m_mobKeys[i] = mob->spatialIndexKey;
        };
        if (n >= 2048 && Core::ParallelWidth() > 1) {
            Core::ParallelFor(n, 512, keyOf);
        } else {
            for (size_t i = 0; i < n; ++i) keyOf(i);
        }
        uint64_t lastKey = 0;
        std::vector<Game::Mob*>* bucket = nullptr;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t key = m_mobKeys[i];
            if (!bucket || key != lastKey) {
                bucket  = &m_byChunk[key];
                lastKey = key;
            }
            bucket->push_back(m_mobList[i]);
        }
    }

    void MobManager::CollectInBoxParallel(const Game::AABB& box, const Game::Entity* except,
                                          std::vector<Game::Entity*>& out) const {
        const int minCX = static_cast<int>(std::floor(box.min.x)) >> 4;
        const int maxCX = static_cast<int>(std::floor(box.max.x)) >> 4;
        const int minCZ = static_cast<int>(std::floor(box.min.z)) >> 4;
        const int maxCZ = static_cast<int>(std::floor(box.max.z)) >> 4;

        std::vector<const std::vector<Game::Mob*>*> buckets;
        size_t total = 0;
        for (int cx = minCX; cx <= maxCX; ++cx) {
            for (int cz = minCZ; cz <= maxCZ; ++cz) {
                const auto it = m_byChunk.find(ChunkKey(cx, cz));
                if (it == m_byChunk.end() || it->second.empty()) continue;
                buckets.push_back(&it->second);
                total += it->second.size();
            }
        }
        if (total < 4096 || Core::ParallelWidth() <= 1) {
            for (const auto* bucket : buckets) {
                for (Game::Mob* mob : *bucket) {
                    if (mob == except || mob->IsRemoved()) continue;
                    if (!mob->GetAABB().Intersects(box)) continue;
                    out.push_back(mob);
                }
            }
            return;
        }

        std::vector<std::vector<Game::Entity*>> perBucket(buckets.size());
        Core::ParallelFor(buckets.size(), 1, [&](size_t i) {
            auto& dst = perBucket[i];
            dst.reserve(buckets[i]->size());
            for (Game::Mob* mob : *buckets[i]) {
                if (mob == except || mob->IsRemoved()) continue;
                if (!mob->GetAABB().Intersects(box)) continue;
                dst.push_back(mob);
            }
        });
        size_t add = 0;
        for (const auto& v : perBucket) add += v.size();
        out.reserve(out.size() + add);
        for (const auto& v : perBucket) out.insert(out.end(), v.begin(), v.end());
    }

    void MobManager::CollectInBox(const Game::AABB& box, const Game::Entity* except,
                                  std::vector<Game::Entity*>& out) const {
        // Chunk range the box spans. Queries are small (a few blocks to a
        // follow range), so this touches a handful of buckets even in a busy
        // world — which is the whole reason the index exists.
        const int minCX = static_cast<int>(std::floor(box.min.x)) >> 4;
        const int maxCX = static_cast<int>(std::floor(box.max.x)) >> 4;
        const int minCZ = static_cast<int>(std::floor(box.min.z)) >> 4;
        const int maxCZ = static_cast<int>(std::floor(box.max.z)) >> 4;

        for (int cx = minCX; cx <= maxCX; ++cx) {
            for (int cz = minCZ; cz <= maxCZ; ++cz) {
                const auto it = m_byChunk.find(ChunkKey(cx, cz));
                if (it == m_byChunk.end()) continue;

                for (Game::Mob* mob : it->second) {
                    if (mob == except || mob->IsRemoved()) continue;
                    if (!mob->GetAABB().Intersects(box)) continue;
                    out.push_back(mob);
                }
            }
        }
    }

    void MobManager::Tick(const ChunkTicketManager* tickets,
                          std::vector<int32_t>& outRemoved) {
        PROFILE_ZONE_N("MobTick");
        PROFILE_PLOT("Server/Mobs", static_cast<int64_t>(m_mobs.size()));

        RebuildIndex();

        int categoryCounts[8] = {};
        int typeCounts[static_cast<size_t>(Game::EntityTypeId::Count)] = {};

        // ── Classification, in parallel ─────────────────────────────────────
        //
        // The per-mob prologue — the entity-ticking hash lookup, the type
        // read, and for primed TNT the whole pre-tick — is pure per-mob work,
        // and running it serially was ~10 ms of the tick at a hundred
        // thousand entities before a single mob had ticked. This pass writes
        // one class byte per mob; the serial loop below then does only what
        // must stay ordered (CheckDespawn can discard, a mob tick can write
        // the world). The long notes that used to sit inline here — the
        // entity-ticking gate being MC ServerLevel.java:371's entityTickList
        // semantics, the CheckDespawn RNG caveat, and why the TNT batch is
        // TNT only — moved with the code they describe.
        //
        //   0 = removed (not counted, not ticked)
        //   1 = counted, chunk not entity-ticking — skipped (vanilla never
        //       despawn-checks a mob outside ticking range)
        //   2 = counted, serial path below
        //   3 = counted, primed TNT fully handled here: CheckDespawn is an
        //       empty override for it, SetOldPosAndRot and tickCount are its
        //       whole pre-tick and are per-mob writes, and it joins the
        //       parallel batch through the atomic cursor. A TNT that is a
        //       vehicle or a passenger takes the serial path like before.
        //   4 = counted, plain falling block (no riding links, does not
        //       hurt what it lands on): eligible for the split tick below —
        //       physics across the pool, landing serial in list order. Only
        //       taken when there are enough of them to pay for the fork/join;
        //       otherwise the serial loop treats it exactly as class 2.
        const size_t mobCount = m_mobList.size();
        m_mobClass.resize(mobCount);
        m_mobTypeIdx.resize(mobCount);
        m_parallelTickBatch.resize(mobCount);
        std::atomic<size_t> tntCount{0};
        {
            PROFILE_ZONE_N("Mob.Classify");
            const auto classify = [&](size_t i) {
                Game::Mob& mob = *m_mobList[i];
                if (mob.IsRemoved()) { m_mobClass[i] = 0; return; }
                m_mobTypeIdx[i] = static_cast<uint16_t>(mob.GetType());
                const Game::Math::ChunkPos mobChunk(
                    static_cast<int>(std::floor(mob.position.x)) >> 4,
                    static_cast<int>(std::floor(mob.position.z)) >> 4);
                // The ticket lookup memoised on the last chunk this runner
                // saw: insertion order puts long runs of mobs in one chunk,
                // and at 1.37M entities the hash per mob was 39 ms a tick.
                // thread_local is right here — each runner keeps its own.
                thread_local Game::Math::ChunkPos t_memoChunk{INT32_MIN, INT32_MIN};
                thread_local bool t_memoTicking = false;
                if (tickets) {
                    if (mobChunk.x != t_memoChunk.x || mobChunk.z != t_memoChunk.z) {
                        t_memoChunk   = mobChunk;
                        t_memoTicking = tickets->IsEntityTickingAfterUpdates(mobChunk);
                    }
                    if (!t_memoTicking) { m_mobClass[i] = 1; return; }
                }
                if (mob.GetType() == Game::EntityTypeId::Tnt &&
                    !mob.IsVehicle() && !mob.IsPassenger()) {
                    mob.SetOldPosAndRot();
                    ++mob.tickCount;
                    m_parallelTickBatch[tntCount.fetch_add(
                        1, std::memory_order_relaxed)] = &mob;
                    m_mobClass[i] = 3;
                    return;
                }
                if (mob.GetType() == Game::EntityTypeId::FallingBlock &&
                    !mob.IsVehicle() && !mob.IsPassenger() &&
                    // An armed anvil's CheckFallDamage hurts the entities in
                    // its box — a cross-entity write, so it stays serial.
                    !static_cast<const Game::FallingBlockEntity&>(mob).HurtsEntities()) {
                    m_mobClass[i] = 4;
                    return;
                }
                m_mobClass[i] = 2;
            };
            if (mobCount >= 2048 && Core::ParallelWidth() > 1) {
                Core::ParallelFor(mobCount, 512, classify);
            } else {
                for (size_t i = 0; i < mobCount; ++i) classify(i);
            }
            m_parallelTickBatch.resize(tntCount.load());
        }

        // Below this the fork/join costs more than the physics it spreads —
        // same crossover the TNT batch uses.
        constexpr size_t kFallingBatchMin = 256;
        // Counted from the class bytes rather than by an atomic in the
        // classify pass: a contended fetch_add per falling block cost that
        // pass 9 ms a tick at a hundred thousand of them. This is a walk of
        // one contiguous byte per mob.
        size_t fallingCount = 0;
        for (size_t i = 0; i < mobCount; ++i) fallingCount += (m_mobClass[i] == 4);
        const bool fallingBatched =
            fallingCount >= kFallingBatchMin && Core::ParallelWidth() > 1;
        m_fallingBatch.clear();
        if (fallingBatched) m_fallingBatch.reserve(fallingCount);

        for (size_t i = 0; i < mobCount; ++i) {
            if (m_mobClass[i] == 0) continue;
            // Counting covers every non-removed mob, exactly as the single
            // loop did — the not-ticking and the batched included.
            // Type histogram only; the category tally is derived from it
            // after the loop (a type's category is fixed), instead of a
            // type-info lookup per mob — ~5 ms a tick at 176k entities.
            const size_t typeIdx = m_mobTypeIdx[i];
            if (typeIdx < static_cast<size_t>(Game::EntityTypeId::Count)) {
                ++typeCounts[typeIdx];
            }
            if (m_mobClass[i] == 4 && fallingBatched) {
                // Collected in order; the pre-tick and both tick halves run
                // below. (CheckDespawn is an empty override for this type.)
                // Nothing on the entity is touched here — this loop must not
                // take a cache miss per falling block.
                m_fallingBatch.push_back(
                    static_cast<Game::FallingBlockEntity*>(m_mobList[i]));
                continue;
            }
            if (m_mobClass[i] != 2 && m_mobClass[i] != 4) continue;
            Game::Mob& mob = *m_mobList[i];

            // MC ServerLevel.java:374-375 — checkDespawn, then tick. See the
            // classification note for the RNG caveat: nothing that skips AI
            // ever draws from CheckDespawn's stream, so the batched TNT not
            // passing through here is stream-neutral.
            mob.CheckDespawn();
            if (mob.IsRemoved()) continue;

            // Passengers are not self-ticked — their vehicle drives them via
            // TickPassengerChain below, mirroring MC ServerLevel.
            if (mob.IsPassenger()) continue;

            // Immediately before tick(), never inside it: the walk animation
            // measures this tick's displacement against these values.
            mob.SetOldPosAndRot();
            ++mob.tickCount;

            mob.Tick();

            // SIMPLIFICATION vs MC: a rider ticks iff its vehicle ticked. MC
            // re-checks the entity tick list per passenger; here the vehicle's
            // chunk answered for the whole stack (they occupy the same spot).
            if (mob.IsVehicle()) TickPassengerChain(mob);
        }

        for (size_t t = 0; t < static_cast<size_t>(Game::EntityTypeId::Count); ++t) {
            if (typeCounts[t] == 0) continue;
            const size_t category = static_cast<size_t>(
                Game::GetEntityTypeInfo(static_cast<Game::EntityTypeId>(t)).category);
            if (category < 8) categoryCounts[category] += typeCounts[t];
        }

        // The deferred half. Runs after every other mob has ticked, which is a
        // reordering — but a TNT's physics reads nothing another mob wrote this
        // tick, and its blast is queued rather than applied, so the only thing
        // that moved is where the CPU time was spent.
        if (!m_parallelTickBatch.empty()) {
            PROFILE_ZONE_N("Mob.TntTickParallel");
            const size_t n = m_parallelTickBatch.size();
            // Below this the fork/join costs more than it saves. A TNT tick is
            // ~1.4 us, a join is tens of us, so a few hundred is the crossover.
            if (n >= 256 && Core::ParallelWidth() > 1) {
                // Grain 64: one claim is a meaningful run of work, and the
                // entities are only READ through their pointers here — the
                // vector itself is not written, so there is no false sharing to
                // size against.
                Core::ParallelFor(n, 64, [this](size_t i) {
                    m_parallelTickBatch[i]->Tick();
                });
            } else {
                for (Game::Mob* m : m_parallelTickBatch) m->Tick();
            }
#ifdef TRACY_ENABLE
            {
                // Diagnostic: how hard a mass detonation is throwing things.
                double maxSpeedSq = 0.0;
                for (const Game::Mob* m : m_parallelTickBatch) {
                    maxSpeedSq = std::max(maxSpeedSq, glm::dot(m->velocity, m->velocity));
                }
                PROFILE_PLOT("Tnt/MaxSpeed", static_cast<int64_t>(std::sqrt(maxSpeedSq)));
            }
#endif
            m_parallelTickBatch.clear();
        }

        // ── Falling blocks: physics in parallel, landing serial in order ───
        //
        // What the serial loop would have done for these is, per entity and
        // in list order: gravity, move, then the landing branch. The move
        // reads the world and writes only the entity; the landing branch
        // writes the world (the entity becomes a block). Two facts make the
        // split exact rather than approximate:
        //
        //   * A same-tick world write that can change an entity's move comes
        //     ONLY from the landing half of an entity EARLIER in the order —
        //     block ticks ran before the mob loop, blasts apply after it, and
        //     the landing half writes exactly one cell (its landing cell).
        //   * Therefore an entity's parallel move is the serial answer unless
        //     a landing cell written before its turn lies inside the region
        //     its move swept. When one does, its physics is rolled back to
        //     the pre-tick snapshot and re-run serially at its turn, against
        //     the world as the serial loop would have seen it.
        //
        // This is what keeps MC's column collapse: the bottom block lands and
        // becomes a block, the one above it re-runs its move, collides with
        // the fresh block, stops a cell higher, lands in turn — the whole
        // column touches down in one tick, exactly as the serial loop did.
        // The retries are a fraction of the batch (only blocks over a cell
        // that filled this tick), and the airborne ticks — every tick of the
        // fall but the last — retry nothing at all.
        //
        // The reordering that IS accepted: like the TNT batch, these tick
        // after every other class-2 mob rather than interleaved with them.
        if (!m_fallingBatch.empty()) {
            PROFILE_ZONE_N("Mob.FallingTickParallel");
            const size_t n = m_fallingBatch.size();
            m_fallingSnapshots.resize(n);
            m_fallingOutcome.resize(n);
            {
                PROFILE_ZONE_N("Mob.FallingPhysics");
                // Grain 64 for the same reason as the TNT batch: the entities
                // are reached through pointers, only the snapshot and outcome
                // slots are written per index. The pre-tick (oldPosition,
                // tickCount) is per-entity too, so it lives here as well.
                Core::ParallelFor(n, 64, [this](size_t i) {
                    Game::FallingBlockEntity* fb = m_fallingBatch[i];
                    fb->SetOldPosAndRot();
                    ++fb->tickCount;
                    m_fallingSnapshots[i] = fb->CapturePhysics();
                    m_fallingOutcome[i] = static_cast<uint8_t>(fb->TickPhysics());
                });
            }
            {
                PROFILE_ZONE_N("Mob.FallingLanding");
                m_writtenColumns.clear();
                const auto columnKey = [](int x, int z) {
                    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
                            static_cast<uint32_t>(z);
                };
                // Conservative superset of MoveEntity's collider region for
                // the move the snapshot describes: the box at the start and at
                // start + (velocity after gravity, clamped to its 16-block
                // step), a block of headroom above, a block below, and a
                // block of slack all round on top of that. A superset only
                // costs a retry that recomputes the same answer.
                // Reads the snapshot only — never the entity — so the airborne
                // ticks, where nothing was written and this is skipped, and
                // the landing ticks, where it runs for every entity, both
                // stay off the per-entity cache-miss path.
                const auto conflicts = [&](const Game::FallingBlockEntity::PhysicsSnapshot& snap) {
                    constexpr double kMaxStep = 16.0;
                    const glm::vec3 half = snap.half;
                    glm::dvec3 d = snap.velocity;
                    d.y -= snap.gravity;
                    d = glm::dvec3(std::clamp(d.x, -kMaxStep, kMaxStep),
                                   std::clamp(d.y, -kMaxStep, kMaxStep),
                                   std::clamp(d.z, -kMaxStep, kMaxStep));
                    const glm::dvec3 a = snap.position;
                    const glm::dvec3 b = snap.position + d;
                    const int x0 = static_cast<int>(std::floor(std::min(a.x, b.x) - half.x)) - 1;
                    const int x1 = static_cast<int>(std::floor(std::max(a.x, b.x) + half.x)) + 1;
                    const int z0 = static_cast<int>(std::floor(std::min(a.z, b.z) - half.z)) - 1;
                    const int z1 = static_cast<int>(std::floor(std::max(a.z, b.z) + half.z)) + 1;
                    const int y0 = static_cast<int>(std::floor(std::min(a.y, b.y))) - 2;
                    const int y1 = static_cast<int>(std::floor(std::max(a.y, b.y) +
                                                               2.0 * half.y)) + 2;
                    for (int x = x0; x <= x1; ++x) {
                        for (int z = z0; z <= z1; ++z) {
                            const auto it = m_writtenColumns.find(columnKey(x, z));
                            if (it == m_writtenColumns.end()) continue;
                            if (it->second.first <= y1 && it->second.second >= y0) return true;
                        }
                    }
                    return false;
                };
                using Outcome = Game::FallingBlockEntity::PhysicsOutcome;
                size_t retries = 0;
                for (size_t i = 0; i < n; ++i) {
                    if (!m_writtenColumns.empty() && conflicts(m_fallingSnapshots[i])) {
                        Game::FallingBlockEntity* fb = m_fallingBatch[i];
                        fb->RestorePhysics(m_fallingSnapshots[i]);
                        m_fallingOutcome[i] = static_cast<uint8_t>(fb->TickPhysics());
                        ++retries;
                    }
                    if (m_fallingOutcome[i] != static_cast<uint8_t>(Outcome::NeedsLanding)) continue;
                    Game::FallingBlockEntity* fb = m_fallingBatch[i];
                    glm::ivec3 cell;
                    if (fb->TickLanding(&cell)) {
                        const auto [it, fresh] = m_writtenColumns.try_emplace(
                            columnKey(cell.x, cell.z), cell.y, cell.y);
                        if (!fresh) {
                            it->second.first  = std::min(it->second.first,  cell.y);
                            it->second.second = std::max(it->second.second, cell.y);
                        }
                    }
                }
                PROFILE_PLOT("Falling/Batch", static_cast<int64_t>(n));
                PROFILE_PLOT("Falling/Retries", static_cast<int64_t>(retries));
            }
            m_fallingBatch.clear();
        }

        // ── This tick's detonations ────────────────────────────────────────
        //
        // HERE, and the position is load-bearing at both ends. AFTER the tick
        // loop, because that loop is what queues them. BEFORE the sweep at the
        // bottom, because a blast's apply reads params.source — the primed TNT
        // that produced it — and the sweep is what frees it. And before
        // DrainSpawned, so a TNT block the blast primes is absorbed on this
        // tick rather than idling until the next.
        if (m_level) m_level->ResolveQueuedExplosions();

        // Absorb anything created during the tick (breeding, reinforcements).
        // Deferred by ServerLevelBridge::AddFreshEntity precisely so the loop
        // above never mutates the container it is iterating.
        if (m_level) {
            auto& spawned = m_level->DrainSpawned();
            for (auto& entity : spawned) {
                // Only mobs are accepted here; a non-mob entity would have no
                // manager to tick it, so dropping it is better than leaking it
                // into a container that will never run its logic.
                if (auto* mob = dynamic_cast<Game::Mob*>(entity.get())) {
                    // Ownership moves from the generic Entity pointer to the
                    // typed one; release() is the transfer, not a leak.
                    (void)entity.release();
                    Add(std::unique_ptr<Game::Mob>(mob));
                }
            }
            spawned.clear();
        }

        // Death drops, AT DEATH — MC LivingEntity.die runs dropAllDeathLoot
        // at deathTime 0 (LivingEntity.java:1444,1478-1485), not when the
        // corpse is removed 20 ticks later. Player-attack kills happen during
        // packet processing, before this world tick, so this pass fires the
        // same server tick as the killing blow; the on-fire state and the
        // 100-tick kill-credit window are therefore read at the moment MC
        // reads them. (The old removal-time drop read fire 20 ticks late and
        // lost the drops of any corpse discarded by RemoveInChunk first.)
        //
        // IsDeadOrDying (health <= 0) rather than the removal reason: only a
        // real death drops — a despawn removal never has health at zero.
        //
        // One parallel scan feeds this pass, the dying list and the sweep's
        // compaction below. The three used to be three separate walks of the
        // node map — a cache miss per mob each, ~5 ms a walk at a hundred
        // thousand entities, to establish that nothing died. The list is
        // walked in full (it includes this tick's spawns) so the flags line
        // up with its indices.
        const size_t liveCount = m_mobList.size();
        m_mobFlags.resize(liveCount);
        {
            PROFILE_ZONE_N("Mob.ScanDying");
            const auto scan = [this](size_t i) {
                const Game::Mob& mob = *m_mobList[i];
                uint8_t f = 0;
                if (mob.IsRemoved())       f |= 1;
                if (mob.IsDeadOrDying())   f |= 2;
                if (mob.HoldsEntityRefs()) f |= 4;
                m_mobFlags[i] = f;
            };
            if (liveCount >= 2048 && Core::ParallelWidth() > 1) {
                Core::ParallelFor(liveCount, 512, scan);
            } else {
                for (size_t i = 0; i < liveCount; ++i) scan(i);
            }
        }
        for (size_t i = 0; i < liveCount; ++i) {
            if (!(m_mobFlags[i] & 2)) continue;
            Game::Mob* mob = m_mobList[i];
            if (!m_deathLootDropped.insert(mob->GetId()).second) continue;
            DropDeathLoot(*mob);
        }

        // ── Clear references BEFORE freeing anything ───────────────────────
        //
        // Mobs cache raw pointers to each other across ticks — a breeding
        // partner, a followed parent, an attack target, the thing being looked
        // at — and re-validate them with IsAlive() on the next tick. That is
        // MC's design, and it is safe there only because the JVM keeps a
        // removed entity's object alive for as long as anything points at it.
        //
        // Here, freeing the mob first turns every one of those IsAlive() calls
        // into a virtual dispatch through a freed vptr. So each dying mob is
        // announced to every survivor first, and only then destroyed.
        //
        // This loop is O(dying x surviving). Its original justification was
        // that "dying is normally zero and rarely more than a handful" — which
        // a mass detonation destroys outright: a thousand corpses against a
        // million survivors is a billion virtual dispatches, each chaining a
        // brain lookup and two goal-selector walks. That is the only quadratic
        // term on the server tick thread.
        //
        // The survivor loop is now skipped for anything that has never
        // acquired a cross-entity pointer, for which ClearReferenceTo is a
        // provable no-op. See Entity::HoldsEntityRefs for why the flag is
        // set-only and where it is set. In a TNT cascade this takes the pass
        // to zero; in a normal world nothing changes, because `dying` really
        // is a handful and every AI mob has the flag set anyway.
        std::vector<Game::Mob*> dying;
        for (size_t i = 0; i < liveCount; ++i) {
            if (m_mobFlags[i] & 1) dying.push_back(m_mobList[i]);
        }

        if (!dying.empty()) {
            PROFILE_ZONE_N("Mob.ClearRefs");
            // From the scan's flags: the survivors that hold references are
            // the only entities touched. The old map walk asked every mob the
            // two questions itself — 38 ms a tick on a landing tick with 70k
            // falling blocks dying, all of it to learn that none of them
            // holds a pointer.
            //
            // EVERY dead mob is announced, the block-shaped ones included. A
            // primed TNT and a falling block are LivingEntities here (see the
            // architecture note in PrimedTnt.hpp), so a look-at or sensing
            // goal can and does cache a pointer to one — filtering them out
            // "because nothing references them" was a real crash: a cow
            // watching a summoned TNT dereferenced its freed vptr in
            // GoalSelector::Tick the tick after it blew up. The cost is
            // O(dying x survivors-with-refs), bounded by the handful of AI
            // mobs near a mass event.
            for (size_t i = 0; i < liveCount; ++i) {
                if ((m_mobFlags[i] & 5) != 4) continue;   // alive AND holds refs
                Game::Mob* survivor = m_mobList[i];
                for (Game::Mob* dead : dying) survivor->ClearReferenceTo(dead);
            }
            // Player views hold no entity pointers of their own, but they are
            // LivingEntities and a future one might — clearing them here keeps
            // the invariant "nothing points at a dying mob" total.
            if (m_level) {
                for (PlayerEntityView* view : m_level->PlayerViews()) {
                    for (Game::Mob* dead : dying) view->ClearReferenceTo(dead);
                }
            }
        }

        // Sweep. Deferred from the tick loop for the same reason MC defers it:
        // a mob's death can remove OTHER mobs (a creeper explosion), and erasing
        // mid-iteration would invalidate the iterator that is still running.
        // Own scope: Tracy's ZoneScopedN always declares a variable called
        // ___tracy_scoped_zone, so two zones in one scope are a redefinition.
        // The enclosing "MobTick" zone is at function scope.
        // ── Keep the spatial index honest, WITHOUT another full rebuild ────
        //
        // BEFORE the sweep, because the sweep frees these mobs and this reads
        // ChunkKeyOf(*dead). The index must hold no stale pointer once this
        // function returns: the natural spawner runs right after and its
        // CheckSpawnObstruction queries CollectInBox, which before any of this
        // existed walked freed pointers and crashed the server thread.
        //
        // PATCHED, not rebuilt. A full rebuild is O(live mobs) and it was the
        // SECOND one in the same tick — 2.17 ms each at a hundred thousand
        // entities, 3.7 ms of a tick spent re-deriving an index that a handful
        // of erases invalidated. `dying` already holds exactly the pointers
        // that went stale, so removing those is O(dying x bucket) instead.
        //
        // Correct for the same reason the rebuild was: every erased mob is in
        // `dying` (the sweep erases exactly what IsRemoved reported, which is
        // what `dying` collected), so no stale pointer survives.
        //
        // Looked up by the key the mob was FILED under (spatialIndexKey), not
        // by its current position: the tick loop ran between the rebuild and
        // here, so a mob that moved across a chunk edge this tick is in its
        // OLD bucket. Recomputing the key found nothing, left the freed pointer
        // in place, and the natural spawner's CollectInBox crashed on it.
        // A mass death — a whole column of sand landing, a cascade's worth of
        // TNT — is the one case where patching costs more than rebuilding:
        // each patch is a linear search of its bucket, and the buckets it
        // searches are exactly the crowded ones. Past the threshold the sweep
        // below is followed by a full RebuildIndex (parallel keys, ~2 ms at
        // 176k) instead; that leaves the same invariant — no stale pointer
        // in the index once this function returns.
        constexpr size_t kPatchLimit = 4096;
        const bool rebuildInsteadOfPatch = dying.size() > kPatchLimit;
        if (!dying.empty() && !rebuildInsteadOfPatch) {
            PROFILE_ZONE_N("Mob.PatchIndex");
            for (Game::Mob* dead : dying) {
                const auto it = m_byChunk.find(dead->spatialIndexKey);
                if (it == m_byChunk.end()) continue;
                auto& bucket = it->second;
                // Order within a bucket carries no meaning — CollectInBox
                // filters by box and the caller sorts if it cares — so
                // swap-and-pop beats erase-shift.
                const auto found = std::find(bucket.begin(), bucket.end(), dead);
                if (found != bucket.end()) {
                    *found = bucket.back();
                    bucket.pop_back();
                }
            }
        }

        if (!dying.empty()) {
            PROFILE_ZONE_N("Mob.Sweep");
            // Order-preserving compaction from the scan's flags — no entity
            // is touched, and the insertion order the tick loop depends on
            // survives (a swap-and-pop would scramble it). Then the map
            // erases, by id, exactly the entries the compaction dropped.
            size_t w = 0;
            for (size_t i = 0; i < liveCount; ++i) {
                if (!(m_mobFlags[i] & 1)) m_mobList[w++] = m_mobList[i];
            }
            m_mobList.resize(w);
            for (Game::Mob* dead : dying) {
                const int32_t id = dead->GetId();
                outRemoved.push_back(id);
                m_deathLootDropped.erase(id);
                m_byUuid.erase(dead->GetUuid());
                m_mobs.erase(id);
            }
            if (rebuildInsteadOfPatch) RebuildIndex();
        }

        // Publish the counts last, so a reader never sees a tally that counts
        // mobs this tick already removed.
        for (int i = 0; i < 8; ++i) {
            m_categoryCounts[i].store(categoryCounts[i], std::memory_order_relaxed);
        }
        for (size_t i = 0; i < static_cast<size_t>(Game::EntityTypeId::Count); ++i) {
            m_typeCounts[i].store(typeCounts[i], std::memory_order_relaxed);
        }
    }

    void MobManager::TickPassengerChain(Game::Entity& vehicle) {
        // Copy — a rider's tick can dismount it (or another rider), which
        // edits the vector being walked.
        const std::vector<Game::Entity*> riders = vehicle.GetPassengers();
        for (Game::Entity* rider : riders) {
            // MC ServerLevel.tickPassenger's guard: only a live rider whose
            // link still points at THIS vehicle gets a ride tick; anything
            // else is forcibly dismounted.
            if (rider->IsRemoved() || rider->GetVehicle() != &vehicle) {
                rider->StopRiding();
                continue;
            }
            rider->SetOldPosAndRot();
            ++rider->tickCount;
            rider->RideTick();
            if (rider->IsVehicle()) TickPassengerChain(*rider);
        }
    }

    void MobManager::DropDeathLoot(Game::Mob& mob) {
        if (!m_level) return;

        Game::JavaRandom& rng = m_level->Random();

        // MC LivingEntity.dropAllDeathLoot (LivingEntity.java:1478-1479):
        // playerKilled = lastHurtByPlayerMemoryTime > 0, read ONCE and fed to
        // both the loot context and the XP gate.
        const bool killedByPlayer = mob.HasPlayerKillCredit();

        // MC LivingEntity.shouldDropLoot: babies drop nothing, except that
        // Monster overrides this and drops regardless of age (Monster.java:
        // 114) — baby zombies do drop in vanilla.
        const bool isMonster = mob.TypeInfo().category == Game::MobCategory::Monster;
        if (!mob.IsBaby() || isMonster) {
            EvaluateLootTable(mob, killedByPlayer, rng);

            // Sheep wool is not in the generated table: MC expresses it as an
            // `alternatives` entry keyed on the sheep's dye colour, which
            // cannot be a static row. An unsheared sheep drops one wool of
            // its colour.
            if (auto* sheep = dynamic_cast<Game::Sheep*>(&mob)) {
                if (!sheep->IsSheared()) {
                    m_level->SpawnItemDrop(mob.position,
                                           Game::Sheep::WoolItemForColor(sheep->GetColor()), 1);
                }
            }

            // MC dropCustomDeathLoot — the per-mob non-table drops (an
            // enderman's carried block).
            mob.DropCustomDeathLoot(*m_level);
        }

        // MC LivingEntity.dropExperience (LivingEntity.java:1492-1495): XP
        // only inside the player kill-credit window, gated on
        // shouldDropExperience — !isBaby() at the base (LivingEntity.java:
        // 536-538), always true for monsters (Monster.java:110-112).
        // isAlwaysExperienceDropper is player/dragon-fight machinery and
        // never reaches this manager. The award spawns real orb entities —
        // ServerLevelBridge::AwardExperience → ExperienceOrbManager::Award.
        if (killedByPlayer && (isMonster || !mob.IsBaby())) {
            const int xp = mob.GetXpReward();
            if (xp > 0) {
                m_level->AwardExperience(mob.position, xp, mob.LastHurtByPlayerId());
            }
        }
    }

    void MobManager::EvaluateLootTable(Game::Mob& mob, bool killedByPlayer,
                                       Game::JavaRandom& rng) {
        const Game::MobLootTable* table = Game::FindMobLootTable(mob.GetType());
        if (!table) return;

        // Slime-size gates: the slime's slimeball pool is size==1, the magma
        // cube's cream entry is size>=2 (entity_properties type_specific
        // slime). Non-slimes carry no gated rows, so 0 never trips a gate.
        int slimeSize = 0;
        if (const auto* slime = dynamic_cast<const Game::Slime*>(&mob)) {
            slimeSize = slime->GetSize();
        }

        const bool onFire = mob.IsOnFire();

        for (int p = 0; p < table->poolCount; ++p) {
            const Game::MobLootPool& pool = table->pools[p];

            // Pool conditions, in the baked order: killed_by_player first,
            // then the random chance — the wither-skull-shaped pools carry
            // both, and MC only rolls the chance when the kill is credited.
            if (pool.requiresPlayerKill && !killedByPlayer) continue;
            if (pool.sizeMin > 0 &&
                (slimeSize < pool.sizeMin || slimeSize > pool.sizeMax)) continue;
            if (pool.chance < 1.0f && rng.NextFloat() >= pool.chance) continue;

            // MC LootPool.addRandomItems: sample the roll count (the witch's
            // ingredient pool is uniform(1,3))...
            int rolls = pool.minRolls;
            if (pool.maxRolls > pool.minRolls) {
                rolls += rng.NextInt(pool.maxRolls - pool.minRolls + 1);
            }

            for (int r = 0; r < rolls; ++r) {
                // ...then ONE weighted pick per roll among the entries whose
                // own conditions pass. An Items::Air row is MC's
                // `minecraft:empty`: it keeps its weight and drops nothing —
                // that IS the elder guardian's 4-in-5 "no armor-trim
                // template" outcome.
                int totalWeight = 0;
                for (int e = 0; e < pool.entryCount; ++e) {
                    const Game::MobLootEntry& entry = pool.entries[e];
                    if (entry.sizeMin > 0 &&
                        (slimeSize < entry.sizeMin || slimeSize > entry.sizeMax)) continue;
                    totalWeight += entry.weight;
                }
                // No passing entries — a size-1 magma cube's cream pool.
                if (totalWeight <= 0) continue;

                int pick = rng.NextInt(totalWeight);
                const Game::MobLootEntry* chosen = nullptr;
                for (int e = 0; e < pool.entryCount; ++e) {
                    const Game::MobLootEntry& entry = pool.entries[e];
                    if (entry.sizeMin > 0 &&
                        (slimeSize < entry.sizeMin || slimeSize > entry.sizeMax)) continue;
                    pick -= entry.weight;
                    if (pick < 0) { chosen = &entry; break; }
                }
                if (!chosen || chosen->item == Game::Items::Air) continue;

                // Uniform over the inclusive range. The minimum MAY be
                // negative (wither skeleton coal -1..1, magma cream -2..1):
                // MC's way of saying "often nothing" — the SAMPLE is clamped
                // to no-drop, never the range, or those rates inflate by up
                // to 50%.
                const int count = chosen->minCount >= chosen->maxCount
                    ? chosen->minCount
                    : chosen->minCount + rng.NextInt(chosen->maxCount - chosen->minCount + 1);
                if (count <= 0) continue;

                // furnace_smelt: a mob killed while burning drops the cooked
                // form. This is why setting a cow on fire before killing it
                // yields steak.
                const Game::ItemID item =
                    (onFire && chosen->smeltedItem != Game::Items::Air) ? chosen->smeltedItem
                                                                        : chosen->item;
                m_level->SpawnItemDrop(mob.position, item, count);
            }
        }
    }

    void MobManager::RemoveInChunk(Game::Math::ChunkPos chunk,
                                   std::vector<int32_t>& outRemoved) {
        RemoveInChunks(std::vector<Game::Math::ChunkPos>{chunk}, outRemoved);
    }

    void MobManager::RemoveInChunks(const std::vector<Game::Math::ChunkPos>& chunks,
                                    std::vector<int32_t>& outRemoved) {
        if (chunks.empty() || m_mobList.empty()) return;
        std::unordered_set<uint64_t> keys;
        keys.reserve(chunks.size() * 2);
        for (const auto& c : chunks) keys.insert(ChunkKey(c.x, c.z));

        // ONE pass over the list classifies every mob (flag bit 0 = leaving),
        // in parallel when there are enough of them. Everything below reads
        // the flags rather than re-asking the mobs.
        const size_t n = m_mobList.size();
        m_mobFlags.assign(n, 0);
        const auto classify = [&](size_t i) {
            if (keys.count(ChunkKeyOf(*m_mobList[i])) != 0) m_mobFlags[i] = 1;
        };
        if (n >= 2048 && Core::ParallelWidth() > 1) {
            Core::ParallelFor(n, 512, classify);
        } else {
            for (size_t i = 0; i < n; ++i) classify(i);
        }

        // Same ordering rule as Tick's sweep: announce every departure before
        // freeing anything. Chunk unload is in fact the COMMON way a mob that
        // something else is pointing at disappears — a herd straddling a chunk
        // border loses half its members here while the other half still hold
        // FollowParentGoal / BreedGoal pointers to them.
        std::vector<Game::Mob*> dying;
        for (size_t i = 0; i < n; ++i) {
            if (m_mobFlags[i]) dying.push_back(m_mobList[i]);
        }
        if (dying.empty()) return;

        // Riding links first: this path never calls Remove(), so the eject
        // that Remove() would have done happens here. A rider almost always
        // shares its vehicle's chunk (it sits ON it), but the seat offsets can
        // straddle a border — ejecting cleanly handles a surviving half of a
        // stack in either direction.
        for (Game::Mob* dead : dying) {
            dead->EjectPassengers();
            dead->StopRiding();
        }

        // A corpse leaving with its chunk already dropped at death (the pass
        // in Tick), so discarding it here no longer eats the drops — the bug
        // that lost kills at chunk edges. The only way one HASN'T dropped is
        // dying and unloading inside the same server tick; MC would have
        // dropped in die(), so drop now, while the entity still exists.
        for (Game::Mob* dead : dying) {
            if (!dead->IsDeadOrDying()) continue;
            if (!m_deathLootDropped.insert(dead->GetId()).second) continue;
            DropDeathLoot(*dead);
        }

        // Same gate as the pass in Tick(), and this site needs it more: a
        // chunk unload kills EVERY entity in the chunk at once, so `dying` here
        // is whole chunk populations rather than a handful. See
        // Entity::HoldsEntityRefs. Every dead mob is announced — TNT and
        // falling blocks are LivingEntities a goal can point at (see Tick).
        {
            for (size_t i = 0; i < n; ++i) {
                if (m_mobFlags[i]) continue;
                Game::Mob* survivor = m_mobList[i];
                if (!survivor->HoldsEntityRefs()) continue;
                for (Game::Mob* dead : dying) survivor->ClearReferenceTo(dead);
            }
            if (m_level) {
                for (PlayerEntityView* view : m_level->PlayerViews()) {
                    for (Game::Mob* dead : dying) view->ClearReferenceTo(dead);
                }
            }
        }

        // Order-preserving compaction from the flags, then the map erases by
        // id — the same shape as Tick's sweep.
        size_t w = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!m_mobFlags[i]) m_mobList[w++] = m_mobList[i];
        }
        m_mobList.resize(w);
        for (Game::Mob* dead : dying) {
            const int32_t id = dead->GetId();
            outRemoved.push_back(id);
            m_deathLootDropped.erase(id);
            m_byUuid.erase(dead->GetUuid());
            m_mobs.erase(id);
        }

        // Same stale-pointer rule as Tick's sweep: the spatial index may not
        // outlive the mobs it points at, and chunk unload can happen at any
        // point in the server tick relative to index consumers.
        RebuildIndex();
    }

    void MobManager::Clear() {
        m_mobs.clear();
        m_mobList.clear();
        m_byUuid.clear();
        m_byChunk.clear();
        m_deathLootDropped.clear();
        for (auto& c : m_categoryCounts) c.store(0, std::memory_order_relaxed);
        for (auto& c : m_typeCounts) c.store(0, std::memory_order_relaxed);
    }

} // namespace Server
