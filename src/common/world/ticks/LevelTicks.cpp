// File: src/common/world/ticks/LevelTicks.cpp
#include "common/world/ticks/LevelTicks.hpp"

#include "common/world/chunk/Chunk.hpp"
#include "common/world/math/WorldCoordinates.hpp"

#include <algorithm>

namespace Game {

    void LevelTicks::ScheduleTick(const glm::ivec3& pos, BlockID block, int delay,
                                  TickPriority priority) {
        if (!m_resolve) return;

        const Math::ChunkPos cp = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        std::shared_ptr<Chunk> chunk = m_resolve(cp.x, cp.z);
        // No chunk, no appointment. This is not a lost update: World::SetBlock
        // cannot write into an unloaded chunk either, so nothing that could
        // have scheduled here has run.
        if (!chunk) return;

        // MC clamps a negative delay to 0 rather than rejecting it — a
        // same-tick schedule is legal and lands in the current drain pass.
        const int clamped = delay < 0 ? 0 : delay;

        {
            const auto guard = chunk->LockExclusive();
            chunk->BlockTicks().Schedule(ScheduledTick{
                block, pos, m_gameTime + clamped, priority, m_subTickCounter++});
        }

        // The chunk's key goes into the active set once per run of
        // appointments in the same chunk, not once per appointment: a landing
        // sand block schedules its neighbours' ticks in a burst, all in one
        // chunk. m_lastActiveKey is reset wherever m_active loses a key.
        const uint64_t key = ChunkKey(cp.x, cp.z);
        if (key != m_lastActiveKey) {
            m_active.insert(key);
            m_lastActiveKey = key;
        }
    }

    bool LevelTicks::HasScheduledTick(const glm::ivec3& pos, BlockID block) const {
        // Collected-but-not-yet-run counts as pending: a block asking this
        // mid-drain is asking "is something already going to happen here", and
        // the answer is yes even though the record has left its container.
        if (m_toRunSet.count(TickIdentity{pos, block}) != 0) return true;

        if (!m_resolve) return false;
        const Math::ChunkPos cp = Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        std::shared_ptr<Chunk> chunk = m_resolve(cp.x, cp.z);
        if (!chunk) return false;
        const auto guard = chunk->LockShared();
        return chunk->BlockTicks().HasScheduledTick(pos, block);
    }

    void LevelTicks::NoteChunkWithTicks(int chunkX, int chunkZ) {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        m_inbox.push_back(ChunkKey(chunkX, chunkZ));
    }

    void LevelTicks::CollectTicks(int64_t gameTime, int maxToProcess) {
        m_collected.clear();
        if (!m_resolve || m_active.empty()) return;

        // Gather the chunks that have something due AND are simulating, pruning
        // keys whose chunk has gone away or emptied. The snapshot is taken first
        // because the prune mutates m_active.
        m_activeScratch.assign(m_active.begin(), m_active.end());

        struct Source {
            std::shared_ptr<Chunk> chunk;
            uint64_t               key;
            // The chunk's head appointment, cached at the moment the chunk
            // was last locked. The merge heap compares THESE — the old
            // comparator took the chunk's shared lock on every comparison,
            // ~2·log2(chunks) lock/unlock pairs per tick collected, which at
            // a landing's 60k ticks a game tick was the larger half of
            // ProcessBlockUpdates. Refreshed under the same exclusive lock
            // the Poll takes, so it can never be stale when compared.
            ScheduledTick          head;
        };
        std::vector<Source> sources;
        sources.reserve(m_activeScratch.size());

        for (uint64_t key : m_activeScratch) {
            const int cx = KeyX(key);
            const int cz = KeyZ(key);
            std::shared_ptr<Chunk> chunk = m_resolve(cx, cz);
            if (!chunk) { m_active.erase(key); m_lastActiveKey = ~0ull; continue; }

            {
                const auto guard = chunk->LockShared();
                const LevelChunkTicks& ticks = chunk->BlockTicks();
                if (ticks.Empty()) { m_active.erase(key); m_lastActiveKey = ~0ull; continue; }
                // Not simulating: keep the key so the appointments survive until
                // a player comes back, but do not run them. This is MC's
                // LevelTicks tickCheck, and it is what stops a distant sand
                // pillar from being frozen mid-collapse forever.
                if (m_tickCheck && !m_tickCheck(cx, cz)) continue;
                if (ticks.Peek()->triggerTick > gameTime) continue;
                const ScheduledTick head = *ticks.Peek();
                sources.push_back(Source{std::move(chunk), key, head});
                continue;
            }
        }
        if (sources.empty()) return;

        // Merge the per-chunk heaps into one globally ordered stream. Ordering
        // ACROSS chunks matters as much as within one: a sand column that
        // straddles a chunk border must still collapse in scheduling order.
        //
        // The comparator reads each chunk's head under its own shared lock. That
        // is a lock per comparison, which sounds expensive and is not: the heap
        // only ever holds the handful of chunks with work due this tick, and the
        // locks are uncontended unless the saver happens to be walking that exact
        // chunk right now.
        auto headLater = [](const Source& a, const Source& b) {
            return ScheduledTick::DrainOrderLess(b.head, a.head);
        };
        std::make_heap(sources.begin(), sources.end(), headLater);

        while (!sources.empty() && static_cast<int>(m_collected.size()) < maxToProcess) {
            std::pop_heap(sources.begin(), sources.end(), headLater);
            Source src = std::move(sources.back());
            sources.pop_back();

            bool hasMoreDue = false;
            {
                const auto guard = src.chunk->LockExclusive();
                LevelChunkTicks& ticks = src.chunk->BlockTicks();
                const ScheduledTick tick = ticks.Poll();
                m_collected.push_back(tick);
                m_toRunSet.insert(TickIdentity{tick.pos, tick.type});

                const ScheduledTick* next = ticks.Peek();
                hasMoreDue = next && next->triggerTick <= gameTime;
                if (hasMoreDue) src.head = *next;
                if (!next) { m_active.erase(src.key); m_lastActiveKey = ~0ull; }
            }

            if (hasMoreDue) {
                sources.push_back(std::move(src));
                std::push_heap(sources.begin(), sources.end(), headLater);
            }
        }
    }

    int LevelTicks::Tick(int64_t gameTime, int maxToProcess, const Runner& run) {
        m_gameTime = gameTime;

        // Fold in chunks that arrived from disk carrying saved appointments.
        {
            std::lock_guard<std::mutex> lock(m_inboxMutex);
            for (uint64_t key : m_inbox) m_active.insert(key);
            m_inbox.clear();
        }

        CollectTicks(gameTime, maxToProcess);
        if (m_collected.empty()) return 0;

        // Collection order is already drain order — the merge above popped the
        // globally-earliest head every step — so this is a straight walk.
        //
        // Running only AFTER every eligible tick has been pulled out is the
        // whole reason for the two-phase shape: a falling sand block's tick
        // clears its cell, which fires neighbour updates that schedule more
        // ticks. If those landed in this same pass, a column of sand would
        // collapse entirely within one game tick instead of cascading.
        for (const ScheduledTick& tick : m_collected) {
            m_toRunSet.erase(TickIdentity{tick.pos, tick.type});
            run(tick.pos, tick.type);
        }

        const int ran = static_cast<int>(m_collected.size());
        m_collected.clear();
        m_toRunSet.clear();   // belt and braces; the loop above empties it
        return ran;
    }

} // namespace Game
