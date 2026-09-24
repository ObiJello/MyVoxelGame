#include "stdafx.h"
#include "TickQueueFixture.h"
#include <algorithm>
#include <iostream>
int main(){
    for(int origin:{-17,0,15}){
        TickQueueFixture host;console::ScheduledTickQueue queue(host);
        for(int i=0;i<1205;++i){int x=origin+i;host.tiles[{x,64,-1}]=i%4+1;queue.addToTickNextTick(x,64,-1,i%4+1,i%7);queue.forceAddTileTick(x,64,-1,i%4+1,0);}
        std::cout<<origin<<' '<<queue.size()<<' '<<queue.tickPendingTicks(false)<<' '<<host.executed.size()<<'\n';
        host.time+=7;std::cout<<queue.tickPendingTicks(false)<<' '<<host.executed.size()<<'\n';
        auto saved=queue.fetchTicksInChunk(1,-1,false);std::sort(saved.begin(),saved.end(),[](const auto& a,const auto& b){return a.x<b.x;});
        for(auto& td:saved)std::cout<<td.x<<' '<<td.m_delay<<' ';std::cout<<'\n';
        auto removed=queue.fetchTicksInChunk(1,-1,true);std::cout<<removed.size()<<' '<<queue.size()<<'\n';
        queue.setTimeAndAdjustTileTicks(500);queue.tickPendingTicks(true);
        std::uint64_t hash=14695981039346656037ull;for(auto [id,x,y,z]:host.executed)for(int value:{id,x,y,z})hash=(hash^std::uint64_t(value))*1099511628211ull;
        std::cout<<host.time<<' '<<queue.size()<<' '<<host.executed.size()<<' '<<hash<<'\n';
        host.loaded=false;queue.addToTickNextTick(0,64,0,8,5);queue.forceAddTileTick(0,64,0,8,5);queue.tickPendingTicks(true);
        host.instant=true;host.loaded=true;host.tiles[{0,64,0}]=8;queue.addToTickNextTick(0,64,0,8,100);queue.addToTickNextTick(0,64,0,9,100);
        std::cout<<queue.size()<<' '<<host.executed.size()<<' '<<host.checks<<'\n';
        host.instant=false;host.executed.clear();
        for(int x=0;x<4;++x)host.tiles[{x,64,0}]=8;
        for(int x=0;x<3;++x)queue.forceAddTileTick(x,64,0,8,0);
        host.callback=[&](int,int x,int,int){if(x==0){queue.forceAddTileTick(0,64,0,8,0);queue.forceAddTileTick(3,64,0,8,-1);}};
        queue.tickPendingTicks(false);
        std::cout<<"rescheduled "<<queue.size()<<' ';for(auto [id,x,y,z]:host.executed)std::cout<<x<<' ';std::cout<<'\n';
        host.callback={};queue.tickPendingTicks(false);std::cout<<queue.size()<<' '<<host.executed.size()<<'\n';
    }
}
