#include "FenceShape.h"
#include "TerrainMesh.h"
#include "World.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
using namespace console;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Access:BlockShapeAccess {
    std::map<std::tuple<int,int,int>,std::pair<int,int>> cells;
    int getTile(int x,int y,int z)const override{auto p=cells.find({x,y,z});return p==cells.end()?0:p->second.first;}
    int getData(int x,int y,int z)const override{auto p=cells.find({x,y,z});return p==cells.end()?0:p->second.second;}
};
int main(){try{
    Access isolated;isolated.cells[{0,0,0}]={85,0};
    auto shape=consoleFenceRenderShape(isolated,0,0,0);
    require(shape.count==3,"An isolated fence has its post and two rails");
    auto collision=consoleFenceCollisionShape(isolated,0,0,0);
    require(collision.count==1 && collision.boxes[0].y1==1.5,"Fence collision uses original one-and-a-half-block height");
    require(collision.boxes[0].x0==6./16 && collision.boxes[0].x1==10./16,"Isolated fence collision is centered");

    Access joined;joined.cells[{0,0,0}]={85,0};joined.cells[{-1,0,0}]={85,0};joined.cells[{1,0,0}]={1,0};
    shape=consoleFenceRenderShape(joined,0,0,0);collision=consoleFenceCollisionShape(joined,0,0,0);
    require(shape.count==3,"East-west fence keeps two original rails");
    require(collision.boxes[0].x0==0 && collision.boxes[0].x1==1,"Fence joins another fence and a solid cube");
    joined.cells[{0,0,-1}]={107,0};
    require(consoleFenceCollisionShape(joined,0,0,0).boxes[0].z0==0,"Fence joins a fence gate");
    joined.cells[{0,0,1}]={37,0};
    require(consoleFenceCollisionShape(joined,0,0,0).boxes[0].z1==10./16,"Fence does not join a flower");
    joined.cells[{0,0,1}]={53,0};
    require(consoleFenceCollisionShape(joined,0,0,0).boxes[0].z1==10./16,"Fence does not join a stair");
    joined.cells[{0,0,1}]={54,0};
    require(consoleFenceCollisionShape(joined,0,0,0).boxes[0].z1==10./16,"Fence does not join a chest");

    for(int direction=0;direction<4;++direction){
        Access gate;gate.cells[{0,0,0}]={107,direction};
        auto closed=consoleFenceCollisionShape(gate,0,0,0);
        require(closed.count==1 && closed.boxes[0].y1==1.5,"Closed gate blocks at fence height");
        require(consoleFenceRenderShape(gate,0,0,0).count==7,"Closed gate uses original seven cuboids");
        gate.cells[{0,0,0}]={107,direction|4};
        require(consoleFenceCollisionShape(gate,0,0,0).count==0,"Open gate has no collision");
        require(consoleFenceRenderShape(gate,0,0,0).count==8,"Open gate uses original eight cuboids");
    }

    World world;
    require(world.set(20,180,20,static_cast<Block>(85)),"Fence stored");
    require(world.collides({20.5,180.9,20.5}),"Fence collision prevents jumping through its upper half");
    require(world.raycast({18,180.75,20.5},{1,0,0},4).hit,"Fence rails and post are pickable");
    world.set(24,180,20,static_cast<Block>(107));world.setData(24,180,20,0);
    require(world.collides({24.5,180.75,20.5}),"Closed gate blocks passage");
    require(world.raycast({22,180.75,20.5},{1,0,0},4).hit,"Closed gate panel is pickable");
    require(world.useBlock(24,180,20) && world.getData(24,180,20)==4,"Using a gate opens it");
    require(!world.collides({24.5,180.75,20.5}),"Open gate clears collision");
    require(!world.raycast({22,180.75,20.5},{1,0,0},4).hit,"Ray passes through open gate center");
    require(world.useBlock(24,180,20) && world.getData(24,180,20)==0,"Using an open gate closes it");
    world.set(32,179,20,Stone);
    require(world.placeBlock(32,180,20,FenceGate,0,{29.5,180,20.5},0),"Fence gate can be selected and placed");
    require(world.getData(32,180,20)==2,"Placed gate uses the original player-facing direction");
    world.set(28,180,20,static_cast<Block>(113));
    require(world.collides({28.5,180.9,20.5}),"Nether fence shares fence collision");
    world.set(63,180,22,Fence);world.set(64,180,22,Fence);
    world.set(63,180,24,Water);world.set(64,180,24,Water);
    world.set(63,180,30,static_cast<Block>(54));world.set(64,180,30,static_cast<Block>(54));
    auto mesh=buildTerrainMesh(world);
    require(!mesh.opaque.empty(),"Fence and gate mesh generated");
    auto snapshot=world.blockSnapshot();
    TerrainMesh sections;
    auto append=[](auto& target,auto& source){target.insert(target.end(),source.begin(),source.end());};
    for(int z=0;z<8;++z)for(int x=0;x<8;++x){
        auto section=buildTerrainMeshRegion(world,snapshot,x*16,z*16,16,16);
        append(sections.opaque,section.opaque);append(sections.water,section.water);
        append(sections.chests,section.chests);append(sections.chestLids,section.chestLids);
        append(sections.largeChests,section.largeChests);append(sections.largeChestLids,section.largeChestLids);
    }
    auto compare=[&](std::vector<Vertex>& whole,std::vector<Vertex>& pieces){
        auto key=[](const Vertex& v){return std::tuple{v.x,v.y,v.z,v.u,v.v,v.r,v.g,v.b,v.a,v.lightU,v.lightV};};
        auto order=[&](const Vertex& a,const Vertex& b){return key(a)<key(b);};
        std::sort(whole.begin(),whole.end(),order);std::sort(pieces.begin(),pieces.end(),order);
        require(whole.size()==pieces.size(),"Section mesh has all vertices from full window");
        for(std::size_t i=0;i<whole.size();++i)require(key(whole[i])==key(pieces[i]),"Section mesh matches full-window geometry and UVs");
    };
    compare(mesh.opaque,sections.opaque);compare(mesh.water,sections.water);
    compare(mesh.chests,sections.chests);compare(mesh.chestLids,sections.chestLids);
    compare(mesh.largeChests,sections.largeChests);compare(mesh.largeChestLids,sections.largeChestLids);
    std::cout<<"Fence connections, gate interaction, collision, picking and meshes passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
