#pragma once
#include "ScheduledTickQueue.h"
#include <functional>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>
struct TickQueueFixture:console::ScheduledTickHost {
    using Position=std::tuple<int,int,int>;
    std::int64_t time=100;
    bool instant=false,loaded=true;
    int checks=0;
    std::map<Position,int> tiles;
    std::vector<std::tuple<int,int,int,int>> executed;
    std::function<void(int,int,int,int)> callback;
    std::int64_t getTime()const override{return time;}
    void setTime(std::int64_t value)noexcept override{time=value;}
    bool getInstaTick()const override{return instant;}
    bool hasChunksAt(int x0,int y0,int z0,int x1,int y1,int z1)override{
        ++checks;if(x1-x0!=16 || y1-y0!=16 || z1-z0!=16)throw std::runtime_error("Original eight-block tick halo changed");return loaded;
    }
    int getTile(int x,int y,int z)override{auto it=tiles.find({x,y,z});return it==tiles.end()?0:it->second;}
    void tickTile(int id,int x,int y,int z)override{executed.emplace_back(id,x,y,z);if(callback)callback(id,x,y,z);}
};
