#pragma once
#include "BlockRaycaster.h"
#include <array>
#include <map>
#include <tuple>
#include <vector>
class FixtureRayTile:public console::BoxRaycastTile {
    int id;
    AABB* shape(console::BlockRaycaster* ray,int x,int y,int z)override{
        if(id==2){bool top=ray->getData(x,y,z)&8;return AABB::newTemp(0,top?0.5:0,0,1,top?1:0.5,1);}
        return AABB::newTemp(0,0,0,1,1,1);
    }
public:
    explicit FixtureRayTile(int value):id(value){}
    AABB* getAABB(console::BlockRaycaster* ray,int x,int y,int z)override{return id>=3?nullptr:shape(ray,x,y,z)->cloneMove(x,y,z);}
    bool mayPick(int data,bool liquid)override{return id!=3 || (liquid && data==0);}
};
struct RaycastFixture:console::RaycastAccess {
    std::map<std::tuple<int,int,int>,std::pair<int,int>> cells;
    std::vector<std::tuple<int,int,int>> visits;
    std::array<FixtureRayTile,4> tiles{FixtureRayTile(1),FixtureRayTile(2),FixtureRayTile(3),FixtureRayTile(4)};
    int getTile(int x,int y,int z)override{visits.emplace_back(x,y,z);auto it=cells.find({x,y,z});return it==cells.end()?0:it->second.first;}
    int getData(int x,int y,int z)override{auto it=cells.find({x,y,z});return it==cells.end()?0:it->second.second;}
    console::RaycastTile* tileFor(int id)override{return id>=1 && id<=4?&tiles[id-1]:nullptr;}
};
