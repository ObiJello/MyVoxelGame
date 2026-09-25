#include "DroppedItemMesh.h"
#include "BlockShape.h"
#include "ItemIcons.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace console {
namespace {
struct Light {float u,v;};
Light light(int packedLight,int floor=0){
    const int block=std::min(240,std::max(packedLight&0xff,floor));
    return {(float((block>>4)&15)+.5f)/16.f,(float((packedLight>>20)&15)+.5f)/16.f};
}
std::array<float,4> tileUV(int tile){
    return {(tile%16)/16.f,(tile/16)/16.f,(tile%16+1)/16.f,(tile/16+1)/16.f};
}
// Axis-aligned cube faces in TileRenderer order: down, up, north, south, west, east.
constexpr float shades[6]{.5f,1.f,.8f,.8f,.6f,.6f};
// textureTile's face for each cube face (it numbers top 0, bottom 1, west,
// east, north, south).
constexpr int textureFace[6]{1,0,4,5,2,3};
void cubeFace(std::vector<Vertex>& out,int face,const std::array<float,3>& center,float half,float spin,
              const std::array<float,4>& uv,float shade,Light l,float alpha=1){
    static constexpr float corners[6][4][3]{
        {{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1}},
        {{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}},
        {{1,1,-1},{1,-1,-1},{-1,-1,-1},{-1,1,-1}},
        {{-1,1,1},{-1,-1,1},{1,-1,1},{1,1,1}},
        {{-1,1,-1},{-1,-1,-1},{-1,-1,1},{-1,1,1}},
        {{1,1,1},{1,-1,1},{1,-1,-1},{1,1,-1}}};
    const float c=std::cos(spin),s=std::sin(spin);
    const float us[4]{uv[0],uv[0],uv[2],uv[2]},vs[4]{uv[1],uv[3],uv[3],uv[1]};
    std::array<Vertex,4> quad;
    for(int i=0;i<4;++i){
        const float x=corners[face][i][0]*half,y=corners[face][i][1]*half,z=corners[face][i][2]*half;
        quad[i]={center[0]+x*c-z*s,center[1]+y,center[2]+x*s+z*c,us[i],vs[i],shade,shade,shade,alpha,l.u,l.v};
    }
    out.insert(out.end(),{quad[0],quad[1],quad[2],quad[0],quad[2],quad[3]});
}
}
DroppedItemMesh buildDroppedItemMesh(const DroppedItem& item,double yaw,double pitch,int packedLight){
    DroppedItemMesh mesh;
    if(item.id<=0)return mesh;
    const float age=float(item.age);
    const float bob=std::sin(age/10.f+item.bobOffset)*.1f+.1f;
    const float spin=(age/20.f+item.bobOffset);
    const std::array<float,3> center{float(item.position.x),float(item.position.y)+.125f+bob,float(item.position.z)};
    const Light l=light(packedLight);
    const bool block=item.id<256 && solid(static_cast<Block>(item.id)) && !consoleIsPartialBlock(item.id);
    if(block){
        const int data=item.damage>=0 && item.damage<=15?item.damage:0;
        for(int face=0;face<6;++face)
            cubeFace(mesh.terrain,face,{center[0],center[1]+.125f,center[2]},.125f,spin,
                     tileUV(textureTile(static_cast<Block>(item.id),textureFace[face],data)),shades[face],l);
        return mesh;
    }
    int tile;bool terrain=false;
    if(item.id<256){tile=textureTile(static_cast<Block>(item.id),2,item.damage>=0 && item.damage<=15?item.damage:0);terrain=true;}
    else if(item.id==373)tile=(item.damage&0x4000)?154:140; // PotionItem bottle
    else if(item.id==383)tile=153;                          // MonsterPlacerItem base
    else tile=consoleItemIcon(item.id,item.damage).tile;
    if(tile<0)return mesh;
    const auto uv=tileUV(tile);
    // EntityRenderDispatcher faces the camera; the sprite spans .5 blocks.
    const float rightX=std::cos(yaw),rightZ=std::sin(yaw);
    const float upX=-std::sin(yaw)*std::sin(pitch),upY=std::cos(pitch),upZ=std::cos(yaw)*std::sin(pitch);
    const std::array<std::array<float,4>,4> corners{{
        {{-.25f,-.125f,uv[0],uv[3]}},{{.25f,-.125f,uv[2],uv[3]}},
        {{.25f,.375f,uv[2],uv[1]}},{{-.25f,.375f,uv[0],uv[1]}}}};
    std::array<Vertex,4> quad;
    for(int i=0;i<4;++i){
        const auto& c=corners[i];
        quad[i]={center[0]+rightX*c[0]+upX*c[1],center[1]+upY*c[1],center[2]+rightZ*c[0]+upZ*c[1],
                 c[2],c[3],1,1,1,1,l.u,l.v};
    }
    auto& out=terrain?mesh.terrain:mesh.items;
    out.insert(out.end(),{quad[0],quad[1],quad[2],quad[0],quad[2],quad[3]});
    return mesh;
}
std::vector<Vertex> buildFallingBlockMesh(const FallingBlock& block,int packedLight){
    std::vector<Vertex> mesh;
    if(block.tile<=0 || block.tile>255)return mesh;
    const Light l=light(packedLight);
    const std::array<float,3> center{float(block.position.x),float(block.position.y),float(block.position.z)};
    for(int face=0;face<6;++face)
        cubeFace(mesh,face,center,.5f,0,tileUV(textureTile(static_cast<Block>(block.tile),textureFace[face],block.data)),shades[face],l);
    return mesh;
}
PrimedTntMesh buildPrimedTntMesh(const PrimedTntState& tnt,int packedLight){
    PrimedTntMesh mesh;
    const float life=float(tnt.life+1);
    // TntRenderer::render: swell by up to 30% over the last ten ticks.
    float scale=1;
    if(life<10){
        float g=std::clamp(1-life/10.f,0.f,1.f);
        g=g*g;g=g*g;
        scale=1+g*.3f;
    }
    const std::array<float,3> center{float(tnt.position.x),float(tnt.position.y),float(tnt.position.z)};
    const Light l=light(packedLight);
    for(int face=0;face<6;++face)
        cubeFace(mesh.tile,face,center,.5f*scale,0,tileUV(textureTile(static_cast<Block>(46),textureFace[face],0)),shades[face],l);
    // Every other five ticks the same cube again, untextured white at
    // alpha (1 - life/100) * .8, full bright and unshaded.
    if(tnt.life/5%2==0){
        const float br=(1-life/100.f)*.8f;
        const Light full=light(0xf000f0);
        for(int face=0;face<6;++face)cubeFace(mesh.flash,face,center,.5f*scale,0,{0,0,1,1},1,full,br);
    }
    return mesh;
}
std::vector<Vertex> buildDestroyStageMesh(int x,int y,int z,int stage,int packedLight){
    if(stage<0 || stage>9)throw std::invalid_argument("Invalid destroy stage");
    std::vector<Vertex> mesh;
    const Light l=light(packedLight);
    const auto uv=tileUV(240+stage);
    for(int face=0;face<6;++face)
        cubeFace(mesh,face,{x+.5f,y+.5f,z+.5f},.502f,0,uv,shades[face],l,.9f);
    return mesh;
}
}
