// File: src/common/world/ticks/LevelChunkTicks.hpp
//
// MC net.minecraft.world.ticks.LevelChunkTicks — the scheduled-tick queue for
// ONE chunk.
//
// Why per chunk rather than one queue on the world: a tick is a promise about a
// block, and it has to share that block's lifetime. Living on the chunk means
// pending ticks unload when the chunk unloads and serialize when it saves, both
// for free and both correct. A single world-wide queue would keep the position
// alive after its chunk was gone and would need a separate index to save.
//
// `Game::Chunk` is shared with the client, so client chunks carry an
// always-empty container — the same arrangement vanilla has, where ClientLevel
// hands out a BlackholeTickAccess. An empty vector plus an empty hash set is
// not worth a second chunk type to avoid.
#pragma once

#include "common/world/ticks/ScheduledTick.hpp"

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace Game {

    class LevelChunkTicks {
    public:
        // Book an appointment. NO-OP when (pos, type) already has one pending —
        // see the uniqueness note in ScheduledTick.hpp. Returns true when the
        // tick was actually added, which is what lets the world-level index
        // know it may need to re-read the head.
        bool Schedule(const ScheduledTick& tick);

        bool HasScheduledTick(const glm::ivec3& pos, BlockID block) const {
            return m_pending.count(TickIdentity{pos, block}) != 0;
        }

        // Earliest appointment in drain order, or null when empty.
        const ScheduledTick* Peek() const {
            return m_queue.empty() ? nullptr : &m_queue.front();
        }

        // Remove and return the earliest appointment. Undefined when empty —
        // callers Peek first, as the drain loop does.
        ScheduledTick Poll();

        bool   Empty() const { return m_queue.empty(); }
        size_t Size()  const { return m_queue.size(); }

        void Clear() {
            m_queue.clear();
            m_pending.clear();
        }

        // ── Persistence (MC LevelChunkTicks.pack / the SavedTick ctor) ──────
        //
        // Delays are stored RELATIVE to `gameTime`, which is what lets a save
        // be loaded into a world whose clock has moved on without every pending
        // tick firing at once.
        std::vector<SavedTick> Pack(int64_t gameTime) const;

        // Replace the contents from a loaded save. `subTickCounter` is the
        // world's running counter and is advanced one per tick restored, so
        // restored appointments keep a deterministic relative order.
        void Unpack(const std::vector<SavedTick>& saved, int64_t gameTime,
                    int64_t& subTickCounter);

    private:
        // A binary heap ordered by ScheduledTick::DrainOrderLess. std::*_heap
        // builds a MAX-heap, so the comparator is inverted to put the earliest
        // appointment at the front.
        struct HeapGreater {
            bool operator()(const ScheduledTick& a, const ScheduledTick& b) const {
                return ScheduledTick::DrainOrderLess(b, a);
            }
        };

        std::vector<ScheduledTick> m_queue;
        // The uniqueness index. Kept alongside the heap rather than derived
        // from it because the dedupe test runs on every neighbour update and a
        // linear scan of the heap would make a wall of sand quadratic.
        std::unordered_set<TickIdentity, TickIdentityHash> m_pending;
    };

} // namespace Game
