#include "stdafx.h"
#include "TickNextTickData.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_set>

int main(){
    std::vector<TickNextTickData> samples;
    for(int x:{-1000000,-433,-16,-1,0,15,432,1000000})
        for(int z:{-433,-1,0,432})for(int y:{0,127,128,255})for(int id:{0,1,8,55,254}){
            TickNextTickData item(x,y,z,id);item.delay((std::int64_t(x)*z+y+id)%101);
            samples.push_back(item);
        }
    // Equality intentionally ignores due time and insertion order. The original
    // hash set deduplicates position/tile while the tree orders time/sequence.
    std::unordered_set<TickNextTickData,TickNextTickDataKeyHash,TickNextTickDataKeyEq> unique;
    for(const auto& item:samples){unique.insert(item);auto copy=item;copy.delay(1ll<<45);unique.insert(copy);}
    std::set<TickNextTickData,TickNextTickDataKeyCompare> ordered(samples.begin(),samples.end());
    std::uint64_t hash=14695981039346656037ull;
    auto mix=[&](std::int64_t value){hash=(hash^std::uint64_t(value))*1099511628211ull;};
    for(const auto& item:ordered){mix(item.x);mix(item.y);mix(item.z);mix(item.tileId);mix(item.m_delay);mix(item.hashCode());}
    std::cout<<samples.size()<<' '<<unique.size()<<' '<<ordered.size()<<' '<<hash<<'\n';
    for(std::size_t i=0;i<samples.size();++i){const auto& a=samples[i];const auto& b=samples[(i*73+11)%samples.size()];mix(a.compareTo(&b));mix(a.equals(&b));mix(TickNextTickData::eq_test(a,b));}
    std::cout<<hash<<'\n';
}
