#include "TerrainMesh.h"
#include "BlockFaceUV.h"
#include "DoorShape.h"
#include "LadderShape.h"
#include "ChestMesh.h"
#include "SkullMesh.h"
#include "LiquidSurface.h"
#include "LiquidFlow.h"
#include "BiomeTint.h"
#include <cmath>
#include <stdexcept>
namespace console {
namespace {
bool waterBlock(Block b){return b==8 || b==9;}
bool lavaBlock(Block b){return b==10 || b==11;}
bool sameLiquid(Block a,Block b){return (waterBlock(a) && waterBlock(b)) || (lavaBlock(a) && lavaBlock(b));}
}
TerrainMesh buildTerrainMesh(const World& world) {
    auto blocks=world.blockSnapshot();
    return buildTerrainMeshRegion(world,blocks,world.originX(),world.originZ(),World::width,World::depth);
}
TerrainMesh buildTerrainMeshRegion(const World& world,const std::vector<std::uint8_t>& blocks,
                                   int x0,int z0,int regionWidth,int regionDepth) {
    if(blocks.size()!=std::size_t(World::width)*World::height*World::depth ||
       regionWidth<1 || regionDepth<1 || x0<world.originX() || z0<world.originZ() ||
       regionWidth>world.originX()+World::width-x0 || regionDepth>world.originZ()+World::depth-z0)
        throw std::invalid_argument("Terrain mesh region is outside its world snapshot");
    TerrainMesh mesh;
    // One contiguous snapshot avoids a map lookup for every block and each of
    // its six neighbors while retaining the chunk-backed world as the authority.
    auto blockAt=[&](int x,int y,int z){return world.inside(x,y,z)
        ?static_cast<Block>(blocks[((z-world.originZ())*World::width+x-world.originX())*World::height+y]):Air;};
    struct ShapeAccess final:BlockShapeAccess {
        const World& world;
        explicit ShapeAccess(const World& w):world(w){}
        int getTile(int x,int y,int z)const override{return world.get(x,y,z);}
        int getData(int x,int y,int z)const override{return world.getData(x,y,z);}
    } shapeAccess(world);
    struct LiquidAccess final:LiquidFlowAccess {
        const World& world;const std::vector<std::uint8_t>& blocks;Block liquid=Water;
        LiquidAccess(const World& w,const std::vector<std::uint8_t>& b):world(w),blocks(b){}
        Block tile(int x,int y,int z)const{return world.inside(x,y,z)?static_cast<Block>(blocks[((z-world.originZ())*World::width+x-world.originX())*World::height+y]):Air;}
        bool sameLiquid(int x,int y,int z)const override{return console::sameLiquid(tile(x,y,z),liquid);}
        bool solidMaterial(int x,int y,int z)const override{return solid(tile(x,y,z));}
        int data(int x,int y,int z)const override{return world.getData(x,y,z);}
        bool blocksMotion(int x,int y,int z)const override{return solidMaterial(x,y,z);}
        bool ice(int x,int y,int z)const override{return tile(x,y,z)==Ice;}
    } liquidAccess(world,blocks);
    // Top, bottom, west, east, north, south. The winding is outward.
    static const int normals[6][3]={{0,1,0},{0,-1,0},{-1,0,0},{1,0,0},{0,0,-1},{0,0,1}};
    static const float corners[6][4][3]={
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},{{0,0,1},{0,0,0},{1,0,0},{1,0,1}},
        {{0,1,1},{0,1,0},{0,0,0},{0,0,1}},{{1,1,0},{1,1,1},{1,0,1},{1,0,0}},
        {{0,1,0},{1,1,0},{1,0,0},{0,0,0}},{{1,1,1},{0,1,1},{0,0,1},{1,0,1}}};
    static const int indices[]={0,1,2,0,2,3};
    static const float shades[]={1,.5,.6,.6,.8,.8};
    for(int z=z0;z<z0+regionDepth;++z)for(int x=x0;x<x0+regionWidth;++x)for(int y=0;y<World::height;++y){
        Block b=blockAt(x,y,z);if(b==Air)continue;
        if(b==54){
            // ChestRenderer draws from the north/west half only.
            if(blockAt(x-1,y,z)==54 || blockAt(x,y,z-1)==54)continue;
            const bool east=blockAt(x+1,y,z)==54,south=blockAt(x,y,z+1)==54,large=east||south;
            int facing=world.getData(x,y,z);
            // Source recalcLockDir keeps a double chest perpendicular to its seam.
            if(large && ((east && facing!=2 && facing!=3)||(south && facing!=4 && facing!=5)))facing=east?3:5;
            auto chest=buildChestMesh(x,y,z,facing,world.renderLight(x,y,z),large);
            auto& body=large?mesh.largeChests:mesh.chests;auto& lid=large?mesh.largeChestLids:mesh.chestLids;
            body.insert(body.end(),chest.begin(),chest.begin()+72);lid.insert(lid.end(),chest.begin()+72,chest.end());continue;
        }
        if(b==130){
            auto chest=buildChestMesh(x,y,z,world.getData(x,y,z),world.renderLight(x,y,z),false);
            mesh.enderChests.insert(mesh.enderChests.end(),chest.begin(),chest.begin()+72);
            mesh.enderChestLids.insert(mesh.enderChestLids.end(),chest.begin()+72,chest.end());
            continue;
        }
        if(b==144){
            auto skull=world.skullInfo(x,y,z);
            const int type=skull?skull->type:0;
            auto vertices=buildSkullMesh(x,y,z,world.getData(x,y,z)&7,skull?skull->rotation:0,
                                         world.renderLight(x,y,z));
            mesh.skulls[type].insert(mesh.skulls[type].end(),vertices.begin(),vertices.end());
            continue;
        }
        const int data=world.getData(x,y,z);
        if(b==116)mesh.enchantTables.push_back({x,y,z});
        // TileRenderer draws these alpha-cutout blocks as crossed or wall
        // planes. A cube here turns an entire biome's undergrowth into boxes.
        const int id=static_cast<int>(b);
        if(id==6 || id==31 || id==32 || id==37 || id==38 || id==39 || id==40 || id==83 || id==106 || id==111){
            const int tile=textureTile(b,0,data),light=world.renderLight(x,y,z);
            const float lu=(((light>>4)&15)+.5f)/16,lv=(((light>>20)&15)+.5f)/16;
            const int tint=(id==31 && data!=0)?consoleBiomeTint(TintKind::Grass,world.neighboringBiomes(x,z),0):
                id==106?consoleBiomeTint(TintKind::Foliage,world.neighboringBiomes(x,z),0):
                id==111?0x208030:0xffffff;
            const float red=((tint>>16)&255)/255.f,green=((tint>>8)&255)/255.f,blue=(tint&255)/255.f;
            auto plane=[&](const std::array<std::array<float,3>,4>& p){
                static const int bothSides[]={0,1,2,0,2,3,2,1,0,3,2,0};
                static const float uv[4][2]={{0,0},{0,1},{1,1},{1,0}};
                for(int k:bothSides){
                    mesh.opaque.push_back({x+p[k][0],y+p[k][1],z+p[k][2],
                        (tile%16+uv[k][0]*.998f+.001f)/16,
                        (tile/16+uv[k][1]*.998f+.001f)/16,
                        red,green,blue,1,lu,lv});
                }
            };
            if(id==111){
                plane({{{0,.02f,0},{0,.02f,1},{1,.02f,1},{1,.02f,0}}});
            }else if(id==106){
                constexpr float inset=.025f;
                if(data&1)plane({{{0,1,1-inset},{0,0,1-inset},{1,0,1-inset},{1,1,1-inset}}});
                if(data&2)plane({{{inset,1,1},{inset,0,1},{inset,0,0},{inset,1,0}}});
                if(data&4)plane({{{1,1,inset},{1,0,inset},{0,0,inset},{0,1,inset}}});
                if(data&8)plane({{{1-inset,1,0},{1-inset,0,0},{1-inset,0,1},{1-inset,1,1}}});
            }else{
                const float top=id==39 || id==40?.4f:id==32?.8f:1.f;
                plane({{{.15f,top,.15f},{.15f,0,.15f},{.85f,0,.85f},{.85f,top,.85f}}});
                plane({{{.85f,top,.15f},{.85f,0,.15f},{.15f,0,.85f},{.15f,top,.85f}}});
            }
            continue;
        }
        if(b==65 && data>=2 && data<=5){
            const auto quad=consoleLadderQuad(data);const int light=world.renderLight(x,y,z);
            const float lu=(((light>>4)&15)+.5f)/16,lv=(((light>>20)&15)+.5f)/16;
            for(int i:indices){const auto& v=quad[i];mesh.opaque.push_back({x+v.x,y+v.y,z+v.z,(3+v.u*.998f+.001f)/16,(5+v.v*.998f+.001f)/16,1,1,1,1,lu,lv});}
            continue;
        }
        int tint=0xffffff;
        if(b==Grass || b==Leaves || waterBlock(b))tint=consoleBiomeTint(b==Grass?TintKind::Grass:b==Leaves?TintKind::Foliage:TintKind::Water,
            world.neighboringBiomes(x,z),b==Leaves?data:0);
        const bool liquid=waterBlock(b) || lavaBlock(b),lava=lavaBlock(b);
        std::array<float,4> heights{1,1,1,1};
        std::array<LiquidUV,4> topUV{};
        if(liquid){
            liquidAccess.liquid=b;
            heights={consoleLiquidCorner(liquidAccess,x,y,z),consoleLiquidCorner(liquidAccess,x,y,z+1),
                consoleLiquidCorner(liquidAccess,x+1,y,z+1),consoleLiquidCorner(liquidAccess,x+1,y,z)};
            // The original top-face pass lowers the four shared corners slightly.
            if(blockAt(x,y+1,z)!=b)for(auto& value:heights)value-=.001f;
            topUV=consoleLiquidTopUV(static_cast<float>(consoleLiquidSlope(liquidAccess,x,y,z)),lava);
        }
        BlockShape shape;shape.count=1;shape.boxes[0]={0,0,0,1,1,1};
        if(id==116)shape.boxes[0]={0,0,0,1,12.f/16,1};
        if(id==117){
            shape.count=4;
            shape.boxes[0]={7.f/16,0,7.f/16,9.f/16,14.f/16,9.f/16};
            shape.boxes[1]={9.f/16,0,5.f/16,15.f/16,2.f/16,11.f/16};
            shape.boxes[2]={2.f/16,0,1.f/16,8.f/16,2.f/16,7.f/16};
            shape.boxes[3]={2.f/16,0,9.f/16,8.f/16,2.f/16,15.f/16};
        }
        if(id==78)shape.boxes[0]={0,0,0,1,2.f*(1+(data&7))/16.f,1};
        if(id==81)shape.boxes[0]={.0625f,0,.0625f,.9375f,1,.9375f};
        if(id==127){
            const float width=(4+2*(data>>2))/16.f,height=(5+2*(data>>2))/16.f;
            const float lo=.5f-width*.5f,hi=.5f+width*.5f;
            switch(data&3){
            case 0:shape.boxes[0]={lo,.75f-height,1.f/16,hi,.75f,(1.f/16)+width};break;
            case 1:shape.boxes[0]={1.f/16,.75f-height,lo,(1.f/16)+width,.75f,hi};break;
            case 2:shape.boxes[0]={lo,.75f-height,1.f-width-1.f/16,hi,.75f,1.f-1.f/16};break;
            case 3:shape.boxes[0]={1.f-width-1.f/16,.75f-height,lo,1.f-1.f/16,.75f,hi};break;
            }
        }
        const bool partial=consoleIsPartialBlock(b) || id==78 || id==117;
        if(partial && id!=78 && id!=117 && id!=118)shape=consoleRenderShape(shapeAccess,x,y,z);
        for(int piece=0;piece<shape.count;++piece){
        const auto& box=shape.boxes[piece];
        for(int f=0;f<6;++f){
            const bool boundary=f==0?box.y1==1:f==1?box.y0==0:f==2?box.x0==0:f==3?box.x1==1:f==4?box.z0==0:box.z1==1;
            auto n=blockAt(x+normals[f][0],y+normals[f][1],z+normals[f][2]);
            if(liquid){
                if(sameLiquid(n,b) || (f!=0 && (n==Ice || (solid(n) && !consoleIsPartialBlock(n) && n!=Leaves && n!=Glass))))continue;
            }else if(boundary && solid(n) && !consoleIsPartialBlock(n) && n!=Leaves && n!=Glass)continue;
            if((b==Leaves || b==Glass) && n==b)continue;
            const auto faceUV=partial?consoleBoxFaceUV(b,f,data,box):consoleBlockFaceUV(b,f,data);
            int tile=textureTile(b,f,data);
            if(id==117 && piece>0)tile=156;
            bool flipDoor=false;
            if(b==64 || b==71){const auto door=consoleDoorTexture(shapeAccess,x,y,z,f);tile=door.tile;flipDoor=door.flip;}
            float shade=shades[f];std::array<float,4> color{shade,shade,shade,1};
            if((b==Grass && f==0)||b==Leaves || (waterBlock(b) && f!=1)){
                color[0]*=((tint>>16)&255)/255.f;color[1]*=((tint>>8)&255)/255.f;color[2]*=(tint&255)/255.f;
            }
            auto& out=waterBlock(b)?mesh.water:mesh.opaque;
            // Original liquid top samples the liquid cell; other faces sample
            // their neighbor. LiquidTile then combines that cell with the one above.
            const int light=liquid && f==0?world.renderLight(x,y,z,true):
                world.renderLight(x+normals[f][0],y+normals[f][1],z+normals[f][2],liquid);
            // Original tex2 packs block into bits 4..7 and sky into 20..23.
            // Sample texel centers in the 16x16 console light texture.
            const float lightU=(((light>>4)&15)+.5f)/16, lightV=(((light>>20)&15)+.5f)/16;
            for(int k:indices){
                const float px=x+(corners[f][k][0]?box.x1:box.x0);
                const float pz=z+(corners[f][k][2]?box.z1:box.z0);
                float py=y+(corners[f][k][1]?box.y1:box.y0);
                if(liquid && corners[f][k][1]>0){const bool east=corners[f][k][0]>0,south=corners[f][k][2]>0;py=y+heights[east?(south?2:3):(south?1:0)];}
                if(liquid && f==1)py+=.001f;
                float u=(tile%16+((flipDoor?1-faceUV[k].u:faceUV[k].u)*.998f+.001f))/16,v=(tile/16+(faceUV[k].v*.998f+.001f))/16;
                if(liquid && f==0){u=topUV[k].u;v=topUV[k].v;}
                if(liquid && f>=2){
                    const bool east=corners[f][k][0]>0,south=corners[f][k][2]>0;
                    const float height=corners[f][k][1]>0?heights[east?(south?2:3):(south?1:0)]:0;
                    const auto side=consoleLiquidSideUV(height,k==1 || k==2,lava);u=side.u;v=side.v;
                }
                out.push_back({px,py,pz,
                    u,v,color[0],color[1],color[2],color[3],lightU,lightV});}
        }
        }
        if(id==117){
            // TileRenderer's three bottle arms are double-sided radial planes.
            const int packed=world.renderLight(x,y,z),tile=157;
            const float lu=(((packed>>4)&15)+.5f)/16,lv=(((packed>>20)&15)+.5f)/16;
            const float u0=(tile%16+.5f)/16,uEmpty=(tile%16+1.f)/16,uFilled=(tile%16)/16;
            const float v0=(tile/16)/16.f,v1=(tile/16+1.f)/16.f;
            for(int arm=0;arm<3;++arm){
                const float angle=arm*2.f*3.14159265358979323846f/3.f+3.14159265358979323846f*.5f;
                const float x1=.5f+std::sin(angle)*.5f,z1=.5f+std::cos(angle)*.5f;
                const float u1=(data&(1<<arm))?uFilled:uEmpty;
                const std::array<Vertex,4> vertices{{
                    {x+.5f,y+1.f,z+.5f,u0,v0,1,1,1,1,lu,lv},
                    {x+.5f,float(y),z+.5f,u0,v1,1,1,1,1,lu,lv},
                    {x+x1,float(y),z+z1,u1,v1,1,1,1,1,lu,lv},
                    {x+x1,y+1.f,z+z1,u1,v0,1,1,1,1,lu,lv}
                }};
                for(int index:{0,1,2,0,2,3,3,2,1,3,1,0})mesh.opaque.push_back(vertices[index]);
            }
        }
        if(id==118){
            // TileRenderer::tesselateCauldronInWorld overlays four inward
            // wall planes and the basin floor on its cutout outer cube.
            const int packed=world.renderLight(x,y,z);
            const float lu=(((packed>>4)&15)+.5f)/16,lv=(((packed>>20)&15)+.5f)/16;
            auto plane=[&](std::vector<Vertex>& target,const std::array<std::array<float,3>,4>& corners,int tile,bool both){
                const float u0=(tile%16+.001f)/16,u1=(tile%16+.999f)/16;
                const float v0=(tile/16+.001f)/16,v1=(tile/16+.999f)/16;
                const float us[]{u0,u1,u1,u0},vs[]{v0,v0,v1,v1};
                for(int index:{0,1,2,0,2,3}){
                    const auto& p=corners[index];target.push_back({x+p[0],y+p[1],z+p[2],us[index],vs[index],1,1,1,1,lu,lv});
                }
                if(both)for(int index:{3,2,1,3,1,0}){
                    const auto& p=corners[index];target.push_back({x+p[0],y+p[1],z+p[2],us[index],vs[index],1,1,1,1,lu,lv});
                }
            };
            constexpr float inner=2.f/16-1.f/128,outer=1-inner;
            plane(mesh.opaque,{{{inner,0,0},{inner,0,1},{inner,1,1},{inner,1,0}}},154,true);
            plane(mesh.opaque,{{{outer,0,1},{outer,0,0},{outer,1,0},{outer,1,1}}},154,true);
            plane(mesh.opaque,{{{1,0,inner},{0,0,inner},{0,1,inner},{1,1,inner}}},154,true);
            plane(mesh.opaque,{{{0,0,outer},{1,0,outer},{1,1,outer},{0,1,outer}}},154,true);
            plane(mesh.opaque,{{{0,.25f,0},{1,.25f,0},{1,.25f,1},{0,.25f,1}}},139,true);
            if(data>0){
                const float waterHeight=(6+std::min(data,3)*3)/16.f;
                plane(mesh.water,{{{0,waterHeight,0},{1,waterHeight,0},{1,waterHeight,1},{0,waterHeight,1}}},205,false);
            }
        }
    }
return mesh;
}
}
