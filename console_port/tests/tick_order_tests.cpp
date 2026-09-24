#include "TickNextTickData.h"
#include <future>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <vector>
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){try{
    TickNextTickData first(-16,255,-1,8),second(-16,255,-1,8),different(-16,255,-1,9);
    require(first.m_delay==0 && first.delay(1ll<<48)==&first && first.m_delay==(1ll<<48),"Due time is initialized and keeps 64 bits");
    require(first.equals(&second) && !first.equals(nullptr) && !first.equals(&different),"Tick identity uses coordinates and tile, not time/sequence");
    second.delay(first.m_delay);require(first.compareTo(&second)<0 && second.compareTo(&first)>0,"Equal-time updates retain construction order");
    auto copy=first;require(copy.compareTo(&first)==0,"Copies retain insertion order");
    different.delay(std::numeric_limits<std::int64_t>::min());require(different.compareTo(&first)<0,"Time comparison avoids subtraction overflow");
    second.delay(std::numeric_limits<std::int64_t>::max());require(first.compareTo(&second)<0,"Extreme future time sorts correctly");
    std::unordered_set<TickNextTickData,TickNextTickDataKeyHash,TickNextTickDataKeyEq> identities;
    identities.insert(first);identities.insert(second);identities.insert(different);require(identities.size()==2,"Position/tile identity suppresses duplicate schedules despite different due times");
    // Original hash arithmetic deliberately wraps at 32 bits.
    require(TickNextTickData(1,0,0,0).hashCode()==268435456 && TickNextTickData(-1,0,0,0).hashCode()==-268435456,"Signed coordinate hashes retain original low bits");
    require(TickNextTickData(std::numeric_limits<int>::max(),std::numeric_limits<int>::min(),std::numeric_limits<int>::min(),254).hashCode()==-268435202,"Hash wrapping is defined even for extreme coordinates");
    std::vector<std::future<std::vector<TickNextTickData>>> workers;
    for(int worker=0;worker<8;++worker)workers.push_back(std::async(std::launch::async,[worker]{std::vector<TickNextTickData> result;for(int i=0;i<4000;++i)result.emplace_back(worker,i,0,8);return result;}));
    std::set<TickNextTickData,TickNextTickDataKeyCompare> all;
    for(auto& worker:workers){auto values=worker.get();for(std::size_t i=1;i<values.size();++i)require(values[i-1].compareTo(&values[i])<0,"Each worker's insertion order stays increasing");all.insert(values.begin(),values.end());}
    require(all.size()==32000,"Concurrent construction never reuses an insertion sequence");
    std::cout<<"Scheduled update identity, 64-bit ordering, defined hash wrapping and concurrent insertion passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
