// ServerLevel's scheduled-update methods, adapted to explicit simulation hooks.
// Source: original/reference-only/ServerLevel.cpp, with host corrections described
// in docs/PORT_STATUS.md. The original queue has no separate priority field.
#include "ScheduledTickQueue.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <stdexcept>
namespace console {
namespace {
void validate(int x,int y,int z,int id){
    for(int coordinate:{x,y,z})if(coordinate<std::numeric_limits<int>::min()+8 || coordinate>std::numeric_limits<int>::max()-8)
        throw std::out_of_range("Scheduled tick neighborhood overflows coordinates");
    if(id<0 || id>254)throw std::out_of_range("Invalid scheduled tile ID");
}
std::int64_t addTime(std::int64_t time,std::int64_t delta){
    if((delta>0 && time>std::numeric_limits<std::int64_t>::max()-delta) ||
       (delta<0 && time<std::numeric_limits<std::int64_t>::min()-delta))
        throw std::overflow_error("Scheduled tick time overflow");
    return time+delta;
}
}
void ScheduledTickQueue::insert(const TickNextTickData& tick){
    if(tickNextTickSet.find(tick)!=tickNextTickSet.end())return;
    auto [position,added]=tickNextTickList.insert(tick);
    if(!added)throw std::logic_error("Duplicate scheduled tick sequence");
    try{tickNextTickSet.insert(tick);}catch(...){tickNextTickList.erase(position);throw;}
}
void ScheduledTickQueue::addToTickNextTick(int x,int y,int z,int tileId,int tickDelay){
    validate(x,y,z,tileId);std::lock_guard lock(m_tickNextTickCS);
    TickNextTickData td(x,y,z,tileId);int r=8;
    if(host.getInstaTick()){
        if(host.hasChunksAt(x-r,y-r,z-r,x+r,y+r,z+r)){
            int id=host.getTile(x,y,z);
            if(id==td.tileId && id>0)host.tickTile(id,x,y,z);
        }
        return;
    }
    if(host.hasChunksAt(x-r,y-r,z-r,x+r,y+r,z+r)){
        if(tileId>0)td.delay(addTime(host.getTime(),tickDelay));
        insert(td);
    }
}
void ScheduledTickQueue::forceAddTileTick(int x,int y,int z,int tileId,int tickDelay){
    validate(x,y,z,tileId);std::lock_guard lock(m_tickNextTickCS);
    TickNextTickData td(x,y,z,tileId);
    if(tileId>0)td.delay(addTime(host.getTime(),tickDelay));
    insert(td);
}
void ScheduledTickQueue::forceAddTileTicks(std::span<const SavedTileTick> ticks){
    std::lock_guard lock(m_tickNextTickCS);
    if(ticks.empty())return;
    const auto time=host.getTime();
    // Validate the whole batch before allocating sequence IDs or touching either
    // index. Copy-on-commit also leaves the queue intact on allocation failure.
    for(const auto& tick:ticks){validate(tick.x,tick.y,tick.z,tick.tileId);if(tick.tileId>0)addTime(time,tick.delay);}
    auto ordered=tickNextTickList;auto identities=tickNextTickSet;
    for(const auto& tick:ticks){
        TickNextTickData td(tick.x,tick.y,tick.z,tick.tileId);
        if(tick.tileId>0)td.delay(addTime(time,tick.delay));
        if(identities.insert(td).second)ordered.insert(td);
    }
    tickNextTickList.swap(ordered);tickNextTickSet.swap(identities);
}
bool ScheduledTickQueue::tickPendingTicks(bool force){
    std::lock_guard lock(m_tickNextTickCS);
    if(processing)throw std::logic_error("Scheduled tick processing cannot recursively process the queue");
    struct Processing {bool& flag;Processing(bool& value):flag(value){flag=true;}~Processing(){flag=false;}} running(processing);
    if(tickNextTickList.size()!=tickNextTickSet.size())throw std::logic_error("Scheduled tick indices out of sync");
    auto count=std::min(tickNextTickList.size(),std::size_t(MAX_TICK_TILES_PER_TICK));
    auto it=tickNextTickList.begin();
    for(std::size_t i=0;i<count && it!=tickNextTickList.end();++i){
        TickNextTickData td=*it;
        if(!force && td.m_delay>host.getTime())break;
        it=tickNextTickList.erase(it);tickNextTickSet.erase(td);
        // Retain the original iterator's next key across a callback. Chunk
        // extraction may erase that key; never keep a dangling iterator.
        std::optional<TickNextTickData> next;
        if(it!=tickNextTickList.end())next=*it;
        int r=8;
        if(host.hasChunksAt(td.x-r,td.y-r,td.z-r,td.x+r,td.y+r,td.z+r)){
            int id=host.getTile(td.x,td.y,td.z);
            if(id==td.tileId && id>0)host.tickTile(id,td.x,td.y,td.z);
        }
        it=next?tickNextTickList.lower_bound(*next):tickNextTickList.end();
    }
    return !tickNextTickList.empty();
}
std::vector<TickNextTickData> ScheduledTickQueue::fetchTicksInChunk(int chunkX,int chunkZ,bool remove){
    std::lock_guard lock(m_tickNextTickCS);std::vector<TickNextTickData> result;
    const std::int64_t west=std::int64_t(chunkX)*16,east=west+16,north=std::int64_t(chunkZ)*16,south=north+16;
    // Allocate before erasing any records, so allocation failure leaves both
    // indices intact. The original extraction order is the identity hash set.
    for(const auto& td:tickNextTickSet)if(td.x>=west && td.x<east && td.z>=north && td.z<south)result.push_back(td);
    if(remove)for(const auto& td:result){tickNextTickList.erase(td);tickNextTickSet.erase(td);}
    return result;
}
ChunkTickSnapshot ScheduledTickQueue::snapshotTicksInChunk(int chunkX,int chunkZ){
    std::lock_guard lock(m_tickNextTickCS);
    return {host.getTime(),fetchTicksInChunk(chunkX,chunkZ,false)};
}
void ScheduledTickQueue::setTimeAndAdjustTileTicks(std::int64_t newTime){
    std::lock_guard lock(m_tickNextTickCS);
    if(processing)throw std::logic_error("Cannot adjust world time during a scheduled tick callback");
    const auto old=host.getTime();
    // Use unsigned magnitude to handle the entire signed clock domain without
    // subtracting two int64 values whose difference may not be representable.
    const bool forward=newTime>=old;
    const std::uint64_t magnitude=forward?std::uint64_t(newTime)-std::uint64_t(old):std::uint64_t(old)-std::uint64_t(newTime);
    decltype(tickNextTickList) ordered;decltype(tickNextTickSet) identities;
    for(auto td:tickNextTickList){
        if(forward){
            if(magnitude>std::uint64_t(std::numeric_limits<std::int64_t>::max())-std::uint64_t(td.m_delay))throw std::overflow_error("Adjusted scheduled time overflow");
            td.m_delay=std::bit_cast<std::int64_t>(std::uint64_t(td.m_delay)+magnitude);
        }else{
            if(magnitude>std::uint64_t(td.m_delay)-std::uint64_t(std::numeric_limits<std::int64_t>::min()))throw std::overflow_error("Adjusted scheduled time overflow");
            td.m_delay=std::bit_cast<std::int64_t>(std::uint64_t(td.m_delay)-magnitude);
        }
        ordered.insert(td);identities.insert(td);
    }
    host.setTime(newTime);tickNextTickList.swap(ordered);tickNextTickSet.swap(identities);
}
}
