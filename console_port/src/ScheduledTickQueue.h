#pragma once
#include "TickNextTickData.h"
#include <cstdint>
#include <mutex>
#include <set>
#include <span>
#include <unordered_set>
#include <vector>
namespace console {
struct SavedTileTick {int x,y,z,tileId,delay;};
struct ChunkTickSnapshot {
    std::int64_t time;
    std::vector<TickNextTickData> ticks;
};
// Simulation must provide real neighborhood checks, tile lookup, and Tile::tick
// dispatch. The queue never substitutes a no-op for an unported block behavior.
class ScheduledTickHost {
public:
    virtual ~ScheduledTickHost()=default;
    virtual std::int64_t getTime()const=0;
    virtual void setTime(std::int64_t time)noexcept=0;
    virtual bool getInstaTick()const=0;
    virtual bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)=0;
    virtual int getTile(int x,int y,int z)=0;
    virtual void tickTile(int id,int x,int y,int z)=0;
};
class ScheduledTickQueue {
    static constexpr int MAX_TICK_TILES_PER_TICK=1000; // Original Level.h.
    ScheduledTickHost& host;
    mutable std::recursive_mutex m_tickNextTickCS;
    std::set<TickNextTickData,TickNextTickDataKeyCompare> tickNextTickList;
    std::unordered_set<TickNextTickData,TickNextTickDataKeyHash,TickNextTickDataKeyEq> tickNextTickSet;
    bool processing=false;
    void insert(const TickNextTickData& tick);
public:
    // The host outlives the queue. World access, including callback execution,
    // stays on its simulation thread; queue mutations are recursively locked.
    explicit ScheduledTickQueue(ScheduledTickHost& value):host(value){}
    void addToTickNextTick(int x,int y,int z,int tileId,int tickDelay);
    void forceAddTileTick(int x,int y,int z,int tileId,int tickDelay);
    // Load an entire validated save batch atomically, with the same identity and
    // deadline rules as forceAddTileTick. Existing schedules retain precedence.
    void forceAddTileTicks(std::span<const SavedTileTick> ticks);
    bool tickPendingTicks(bool force);
    std::vector<TickNextTickData> fetchTicksInChunk(int chunkX,int chunkZ,bool remove);
    ChunkTickSnapshot snapshotTicksInChunk(int chunkX,int chunkZ);
    void setTimeAndAdjustTileTicks(std::int64_t newTime);
    std::size_t size()const {std::lock_guard lock(m_tickNextTickCS);return tickNextTickList.size();}
};
}
