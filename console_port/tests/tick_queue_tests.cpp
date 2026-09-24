#include "TickQueueFixture.h"
#include <future>
#include <iostream>
#include <limits>
static void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
template<class F>static void rejects(F f){try{f();}catch(const std::logic_error&){return;}catch(const std::overflow_error&){return;}throw std::runtime_error("Expected scheduling rejection");}
int main(){try{
    TickQueueFixture host;console::ScheduledTickQueue queue(host);host.tiles[{0,64,0}]=8;
    queue.addToTickNextTick(0,64,0,8,10);queue.addToTickNextTick(0,64,0,8,0);
    require(queue.size()==1 && queue.tickPendingTicks(false) && host.executed.empty(),"First schedule wins; a duplicate does not advance its due time");
    host.time=110;require(!queue.tickPendingTicks(false) && host.executed.size()==1,"Due tick executes and leaves both indices empty");
    host.loaded=false;queue.addToTickNextTick(0,64,0,8,0);require(queue.size()==0,"Normal schedules require the original loaded halo");
    queue.forceAddTileTick(0,64,0,8,0);require(queue.size()==1,"Save-loading force schedule bypasses the halo");
    require(!queue.tickPendingTicks(false) && host.executed.size()==1,"A missing processing halo discards a tick as in the original");
    host.loaded=true;host.instant=true;queue.addToTickNextTick(0,64,0,8,999);require(queue.size()==0 && host.executed.size()==2,"Instant mode invokes the matching block immediately");
    queue.addToTickNextTick(0,64,0,9,999);require(host.executed.size()==2,"Instant mode ignores changed block identity");host.instant=false;
    for(int x:{-17,-16,-1,0,15,16})queue.forceAddTileTick(x,64,-1,8,30);
    auto extracted=queue.fetchTicksInChunk(-1,-1,false);require(extracted.size()==2 && queue.size()==6,"Chunk extraction uses exact negative-coordinate boundaries");
    queue.setTimeAndAdjustTileTicks(1000);extracted=queue.fetchTicksInChunk(-1,-1,true);
    require(extracted.size()==2 && extracted[0].m_delay==1030 && extracted[1].m_delay==1030 && queue.size()==4,"Clock adjustment updates both indices so saving/removal uses current due times");
    queue.setTimeAndAdjustTileTicks(-1000);require(queue.fetchTicksInChunk(0,-1,false)[0].m_delay==-970,"Backward clock adjustment preserves remaining delay");
    queue.tickPendingTicks(true);require(queue.size()==0,"Forced processing ignores due time and safely skips changed blocks");
    host.time=0;host.executed.clear();for(int i=0;i<1005;++i){host.tiles[{i,64,0}]=8;queue.forceAddTileTick(i,64,0,8,0);}
    require(queue.tickPendingTicks(false) && host.executed.size()==1000 && queue.size()==5,"Original per-pass limit is 1000 records");
    require(!queue.tickPendingTicks(false) && host.executed.size()==1005,"Remaining records drain in the next pass");
    // Reentrant scheduling keeps original traversal order. Removing the next
    // chunk during dispatch must not leave the original raw iterator dangling.
    host.executed.clear();queue.forceAddTileTick(0,64,0,8,0);queue.forceAddTileTick(16,64,0,8,0);queue.forceAddTileTick(32,64,0,8,0);
    host.callback=[&](int,int x,int,int){if(x==0){queue.fetchTicksInChunk(1,0,true);queue.forceAddTileTick(48,64,0,8,0);}};
    queue.tickPendingTicks(false);require(host.executed.size()==3 && std::get<1>(host.executed[1])==32 && std::get<1>(host.executed[2])==48,"Callback removal and insertion keep traversal valid");
    host.callback=[&](int,int,int,int){rejects([&]{queue.tickPendingTicks(false);});rejects([&]{queue.setTimeAndAdjustTileTicks(1);});queue.forceAddTileTick(0,64,0,8,0);};
    queue.forceAddTileTick(0,64,0,8,0);require(queue.tickPendingTicks(false) && queue.size()==1,"Self-rescheduling at the end is deferred to the next pass");
    host.callback=[](int,int,int,int){throw std::runtime_error("fixture tick failure");};
    bool thrown=false;try{queue.tickPendingTicks(false);}catch(const std::runtime_error&){thrown=true;}require(thrown && queue.size()==0,"Callback exception leaves both indices consistent");
    require(std::async(std::launch::async,[&]{return queue.size();}).get()==0,"A callback exception releases the queue lock for another thread");
    host.callback={};queue.forceAddTileTick(0,64,0,8,0);require(!queue.tickPendingTicks(false),"Callback exception releases processing guard and recursive lock");
    host.time=std::numeric_limits<std::int64_t>::max();rejects([&]{queue.forceAddTileTick(0,64,0,8,1);});require(queue.size()==0,"Overflowing deadlines are rejected before insertion");
    host.time=0;queue.forceAddTileTick(0,64,0,8,1);rejects([&]{queue.setTimeAndAdjustTileTicks(std::numeric_limits<std::int64_t>::max());});require(host.time==0 && queue.fetchTicksInChunk(0,0,false)[0].m_delay==1,"Failed time adjustment preserves clock and indices");queue.tickPendingTicks(true);
    host.time=std::numeric_limits<std::int64_t>::min();queue.forceAddTileTick(0,64,0,8,0);queue.setTimeAndAdjustTileTicks(std::numeric_limits<std::int64_t>::max());require(queue.fetchTicksInChunk(0,0,false)[0].m_delay==host.time,"Clock adjustment handles unsigned full-domain differences");queue.setTimeAndAdjustTileTicks(std::numeric_limits<std::int64_t>::min());require(queue.fetchTicksInChunk(0,0,false)[0].m_delay==host.time,"Full-domain backward clock adjustment works");
    rejects([&]{queue.forceAddTileTick(std::numeric_limits<int>::max(),0,0,8,0);});rejects([&]{queue.forceAddTileTick(0,0,0,255,0);});
    std::cout<<"Original scheduling, forced/instant updates, extraction, limits, callback mutations and clock repair passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
