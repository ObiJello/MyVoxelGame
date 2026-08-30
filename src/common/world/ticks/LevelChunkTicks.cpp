// File: src/common/world/ticks/LevelChunkTicks.cpp
#include "common/world/ticks/LevelChunkTicks.hpp"

#include <algorithm>

namespace Game {

    bool LevelChunkTicks::Schedule(const ScheduledTick& tick) {
        const TickIdentity id{tick.pos, tick.type};
        if (!m_pending.insert(id).second) return false;   // already booked

        m_queue.push_back(tick);
        std::push_heap(m_queue.begin(), m_queue.end(), HeapGreater{});
        return true;
    }

    ScheduledTick LevelChunkTicks::Poll() {
        std::pop_heap(m_queue.begin(), m_queue.end(), HeapGreater{});
        const ScheduledTick out = m_queue.back();
        m_queue.pop_back();
        m_pending.erase(TickIdentity{out.pos, out.type});
        return out;
    }

    std::vector<SavedTick> LevelChunkTicks::Pack(int64_t gameTime) const {
        std::vector<SavedTick> out;
        out.reserve(m_queue.size());
        for (const ScheduledTick& t : m_queue) {
            out.push_back(SavedTick{
                t.type, t.pos,
                static_cast<int32_t>(t.triggerTick - gameTime),
                t.priority});
        }
        return out;
    }

    void LevelChunkTicks::Unpack(const std::vector<SavedTick>& saved, int64_t gameTime,
                                 int64_t& subTickCounter) {
        Clear();
        m_queue.reserve(saved.size());
        for (const SavedTick& s : saved) {
            Schedule(s.Unpack(gameTime, subTickCounter++));
        }
    }

} // namespace Game
