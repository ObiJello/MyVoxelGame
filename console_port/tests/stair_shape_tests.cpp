#include "BlockShape.h"
#include "World.h"
#include "TerrainMesh.h"
#include "BlockFaceUV.h"
#include <cmath>
#include <map>
#include <tuple>
#include <stdexcept>
#include <iostream>
using namespace console;
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Access final:BlockShapeAccess {
    std::map<std::tuple<int,int,int>,std::pair<int,int>> cells;
    int getTile(int x,int y,int z)const override{auto it=cells.find({x,y,z});return it==cells.end()?0:it->second.first;}
    int getData(int x,int y,int z)const override{auto it=cells.find({x,y,z});return it==cells.end()?0:it->second.second;}
};
static bool occupied(const BlockShape& shape,double x,double y,double z){
    for(int i=0;i<shape.count;++i){auto b=shape.boxes[i];if(x>b.x0&&x<b.x1&&y>b.y0&&y<b.y1&&z>b.z0&&z<b.z1)return true;}
    return false;
}
static int octants(const BlockShape& shape){
    int result=0;
    for(double x:{.25,.75})for(double y:{.25,.75})for(double z:{.25,.75})result+=occupied(shape,x,y,z);
    return result;
}
int main(){try{
    const int ids[]={53,67,108,109,114,128,134,135,136,156};
    for(int id:ids)require(consoleIsStair(id),"Original stair ID registered");
    require(!consoleIsStair(44)&&!consoleIsStair(98),"Slabs and stone bricks are not stairs");
    for(int id:ids)for(int data=0;data<8;++data){
        Access level;level.cells[{0,0,0}]={id,data};
        auto shape=consoleStairShape(level,0,0,0);
        require(shape.count==2&&octants(shape)==6,"Straight stairs fill six octants");
        for(double x:{.25,.75})for(double z:{.25,.75}){
            require(occupied(shape,x,(data&4)?.75:.25,z),"Base occupies entire half block");
            const bool upper=(data&3)==0?x>.5:(data&3)==1?x<.5:(data&3)==2?z>.5:z<.5;
            require(occupied(shape,x,(data&4)?.25:.75,z)==upper,"Riser follows original direction metadata");
        }
    }
    // Neighbor east/north forms an outer corner. The lock on the opposite
    // side restores a straight step; different upside-down bits do not join.
    for(int upside:{0,4}){
        Access level;level.cells[{0,0,0}]={53,upside};level.cells[{1,0,0}]={67,3|upside};
        auto outer=consoleStairShape(level,0,0,0);
        require(octants(outer)==5,"Mixed-material outer corner removes one octant");
        require(occupied(outer,.75,(upside?.25:.75),.25),"Outer corner keeps northeast riser");
        level.cells[{0,0,1}]={109,upside};
        require(octants(consoleStairShape(level,0,0,0))==6,"Matching lock prevents outer corner");
        level.cells.erase({0,0,1});level.cells[{1,0,0}]={67,3|(upside^4)};
        require(octants(consoleStairShape(level,0,0,0))==6,"Opposite vertical half cannot form corner");
        level.cells.erase({1,0,0});level.cells[{-1,0,0}]={135,3|upside};
        auto inner=consoleStairShape(level,0,0,0);
        require(inner.count==3&&octants(inner)==7,"Inner corner adds eighth-block piece");
        level.cells[{0,0,-1}]={156,upside};
        require(octants(consoleStairShape(level,0,0,0))==6,"Matching lock prevents inner corner");
    }
    // Integration with the actual player AABB, including exact top contact.
    World world;world.set(20,180,20,static_cast<Block>(53));world.setData(20,180,20,0);
    require(!world.collides({20.1,180.5,20.5}),"Player can stand on lower stair half");
    require(world.collides({20.1,180.49,20.5}),"Player cannot penetrate lower stair half");
    require(world.collides({20.5,180.5,20.5}),"Player body intersects riser");
    require(!world.collides({20.75,181,20.5}),"Player can stand on upper step");
    require(world.raycast({19,180.75,20.25},{1,0,0},1.49).hit==false,"Ray passes through empty upper stair half");
    auto picked=world.raycast({19,180.75,20.25},{1,0,0},2);
    require(picked.hit && picked.x==20 && picked.px==19,"Ray hits the internal stair riser with correct face");
    require(world.raycast({19,180.25,20.25},{1,0,0},1.1).hit,"Ray hits lower stair half");
    const auto mesh=buildTerrainMesh(world);
    require(mesh.opaque.size()==72,"Stair mesh emits both original cuboids");
    bool lowerTop=false;
    for(const auto& vertex:mesh.opaque){
        require(vertex.x>=20 && vertex.x<=21 && vertex.y>=180 && vertex.y<=181 && vertex.z>=20 && vertex.z<=21,"Stair mesh stays inside its block");
        if(vertex.y==180.5f)lowerTop=true;
        int tile=int(std::floor(vertex.u*16))+16*int(std::floor(vertex.v*16));
        require(tile==textureTile(Planks,0,0),"Wood stair uses its original base texture");
    }
    require(lowerTop,"Stair mesh contains actual half-height vertices");
    auto side=consoleBoxFaceUV(Stone,2,0,{0,0,0,1,.5,1});
    float lo=1,hi=0;for(auto uv:side){lo=std::min(lo,uv.v);hi=std::max(hi,uv.v);}
    require(lo==.5f && hi==1,"Lower half step clips texture instead of stretching it");
    for(int face=0;face<6;++face){
        require(textureTile(static_cast<Block>(134),face,7)==textureTile(Planks,face,1),"Spruce stair preserves base species independently of facing");
        require(textureTile(static_cast<Block>(128),face,3)==textureTile(Sandstone,face,0),"Sandstone stair preserves top and bottom textures");
    }
    for(int data=0;data<16;++data){
        Access level;level.cells[{0,0,0}]={44,data};
        auto slab=consoleCollisionShape(level,0,0,0);
        require(slab.count==1&&octants(slab)==4,"Half slab fills four octants");
        require(occupied(slab,.25,(data&8)?.75:.25,.25),"Slab bit 3 selects top half");
        level.cells[{0,0,0}]={43,data};
        require(octants(consoleCollisionShape(level,0,0,0))==8,"Double slab fills whole block");
    }
    require(world.set(24,180,20,static_cast<Block>(44)),"Half slab is editable");
    require(!world.collides({24.5,180.5,20.5}),"Player stands on bottom slab");
    require(!world.raycast({23,180.75,20.5},{1,0,0},2).hit,"Ray passes over bottom slab");
    world.setData(24,180,20,8);
    require(world.collides({24.5,180.5,20.5}),"Top slab occupies upper half");
    require(world.raycast({23,180.75,20.5},{1,0,0},2).hit,"Ray picks upper slab");
    require(textureTile(static_cast<Block>(43),2,8)==6 && textureTile(static_cast<Block>(44),2,8)==5,"Double slab top-only flag does not alter half slab side texture");
    const auto slabMesh=buildTerrainMesh(world);bool slabBottom=false;
    for(const auto& v:slabMesh.opaque)if(v.x>=24 && v.x<=25){require(v.y>=180.5f,"Upper slab mesh leaves lower half empty");if(v.y==180.5f)slabBottom=true;}
    require(slabBottom,"Upper slab mesh includes its inset bottom face");
    // A neighboring full block must not hide an inset surface in this cell.
    world.set(24,179,20,Stone);
    const auto covered=buildTerrainMesh(world);bool insetVisible=false;
    for(std::size_t i=0;i<covered.opaque.size();i+=3){bool inset=true;
        for(int j=0;j<3;++j){const auto& v=covered.opaque[i+j];inset=inset && v.x>=24 && v.x<=25 && v.y==180.5f;}
        insetVisible=insetVisible||inset;
    }
    require(insetVisible,"Solid neighbor does not cull slab's inset face");
    std::cout<<"Original stair shapes, corner locks, inversion and player collision passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
