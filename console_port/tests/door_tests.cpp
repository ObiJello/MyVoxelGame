#include "DoorShape.h"
#include "LadderShape.h"
#include <cmath>
#include "World.h"
#include "TerrainMesh.h"
#include <map>
#include <tuple>
#include <iostream>
#include <stdexcept>
using namespace console;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Access:BlockShapeAccess {
    std::map<std::tuple<int,int,int>,std::pair<int,int>> cells;
    int getTile(int x,int y,int z)const override{auto p=cells.find({x,y,z});return p==cells.end()?0:p->second.first;}
    int getData(int x,int y,int z)const override{auto p=cells.find({x,y,z});return p==cells.end()?0:p->second.second;}
};
int main(){try{
    for(int id:{64,71})for(int dir=0;dir<4;++dir)for(int open:{0,4})for(int hinge:{0,1}){
        Access level;level.cells[{0,0,0}]={id,dir|open};level.cells[{0,1,0}]={id,8|hinge};
        require(consoleDoorData(level,0,0,0)==(dir|open|(hinge?16:0)),"Lower composite metadata");
        require(consoleDoorData(level,0,1,0)==(dir|open|8|(hinge?16:0)),"Upper composite metadata");
        auto lower=consoleDoorShape(level,0,0,0).boxes[0],upper=consoleDoorShape(level,0,1,0).boxes[0];
        require(lower.x0==upper.x0 && lower.z0==upper.z0 && lower.x1==upper.x1 && lower.z1==upper.z1,"Both halves agree on orientation");
        require((lower.x1-lower.x0)*(lower.z1-lower.z0)==3./16,"Door has original three-pixel thickness");
        require(lower.y0==0 && lower.y1==1,"Each half stays one block high");
        for(int face=0;face<6;++face){
            auto bottom=consoleDoorTexture(level,0,0,0,face),top=consoleDoorTexture(level,0,1,0,face);
            require(bottom.tile==(id==64?97:98),"Lower door texture");
            require(top.tile==(face<2?(id==64?97:98):(id==64?81:82)),"Upper door and edge textures");
            require(bottom.flip==top.flip,"Both door halves agree on texture reflection");
        }
        if(!open){
            require((dir==0?lower.x0==0&&lower.x1==3./16:dir==1?lower.z0==0&&lower.z1==3./16:dir==2?lower.x0==13./16&&lower.x1==1:lower.z0==13./16&&lower.z1==1),"Closed door lies on source direction edge");
        }
    }
    World world;
    require(world.set(20,180,20,static_cast<Block>(64)),"Wooden lower door stored");
    require(world.set(20,181,20,static_cast<Block>(64))&&world.setData(20,181,20,8),"Wooden upper door stored");
    require(world.collides({19.95,180,20.5}),"Closed door blocks passage");
    require(world.raycast({19,180.75,20.5},{1,0,0},2).hit,"Closed door is pickable");
    require(world.useBlock(20,181,20)&&world.getData(20,180,20)==4,"Using upper half opens lower state");
    require(!world.collides({20.5,180,20.5}),"Open door leaves passage clear");
    require(!world.raycast({19,180.75,20.5},{1,0,0},2).hit,"Ray passes through open doorway");
    auto mesh=buildTerrainMesh(world);require(!mesh.opaque.empty(),"Door mesh generated");
    for(const auto& v:mesh.opaque)require(v.z>=20 && v.z<=20.1875f,"Open door mesh uses thin panel bounds");
    require(world.useBlock(20,180,20)&&world.getData(20,180,20)==0,"Using lower half closes door");
    world.set(24,180,20,static_cast<Block>(71));world.set(24,181,20,static_cast<Block>(71));world.setData(24,181,20,8);
    require(world.useBlock(24,180,20)&&world.getData(24,180,20)==0,"Iron door consumes use without hand-opening");
    require(!world.useBlock(30,180,20),"Ordinary blocks do not consume use");
    for(int data=2;data<=5;++data){
        auto box=consoleLadderShape(data).boxes[0];
        require((box.x1-box.x0)*(box.z1-box.z0)==.125,"Original ladder collision thickness");
        auto quad=consoleLadderQuad(data);
        for(const auto& v:quad){
            require(v.x>=0 && v.x<=1 && v.y>=0 && v.y<=1 && v.z>=0 && v.z<=1,"Ladder plane stays inside block");
            require(std::abs((data==2?v.z:data==3?v.z:data==4?v.x:v.x)-(data==2 || data==4?.95f:.05f))<.000001f,"Ladder plane is offset from support wall");
        }
    }
    const auto falling=consoleLadderVelocity({20,-30,-20},false);
    require(std::abs(falling.x-3)<.000001 && falling.y==-3 && std::abs(falling.z+3)<.000001,"Ladder limits horizontal motion and falling speed");
    require(consoleLadderVelocity({0,-3,0},true).y==0,"Sneaking holds ladder height");
    world.set(28,180,20,static_cast<Block>(65));world.setData(28,180,20,5);
    require(!world.collides({28.5,180,20.5}),"Ladder leaves room for player's body");
    require(world.collides({28.1,180,20.5}),"Ladder panel collides near support");
    require(world.raycast({29.5,180.75,20.5},{-1,0,0},2).hit,"Ladder is pickable");
    require(world.breakBlock(20,181,20) && world.get(20,180,20)==Air && world.get(20,181,20)==Air,"Mining upper door removes both halves");
    require(world.breakBlock(24,180,20) && world.get(24,181,20)==Air,"Mining lower door removes upper half");
    std::cout<<"Door metadata, shapes, textures, picking and opening passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
