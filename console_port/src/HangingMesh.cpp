#include "HangingMesh.h"
#include "ItemIcons.h"
#include <array>
#include <cmath>

namespace console {
namespace {
struct Motive {const wchar_t* name;int width,height,u,v;};
// Painting::Motive::values, Minecraft.World/Painting.cpp.
constexpr Motive motives[]{
    {L"Kebab",16,16,0,0},{L"Aztec",16,16,16,0},{L"Alban",16,16,32,0},
    {L"Aztec2",16,16,48,0},{L"Bomb",16,16,64,0},{L"Plant",16,16,80,0},
    {L"Wasteland",16,16,96,0},{L"Pool",32,16,0,32},{L"Courbet",32,16,32,32},
    {L"Sea",32,16,64,32},{L"Sunset",32,16,96,32},{L"Creebet",32,16,128,32},
    {L"Wanderer",16,32,0,64},{L"Graham",16,32,16,64},
    {L"Match",32,32,0,128},{L"Bust",32,32,32,128},{L"Stage",32,32,64,128},
    {L"Void",32,32,96,128},{L"SkullAndRoses",32,32,128,128},{L"Wither",32,32,160,128},
    {L"Fighters",64,32,0,96},{L"Pointer",64,64,0,192},{L"Pigscene",64,64,64,192},
    {L"BurningSkull",64,64,128,192},{L"Skeleton",64,48,192,64},{L"DonkeyKong",64,48,192,112}
};
struct Point {float x,y,z;};
struct Tex {float u,v;};
void quad(std::vector<Vertex>& mesh,const std::array<Point,4>& points,const std::array<Tex,4>& uv,
          Point center,int dir,int packedLight){
    // HangingEntity::setDir uses opposite facing for north/south paintings.
    constexpr float cosine[]{-1,0,1,0};
    constexpr float sine[]{0,-1,0,1};
    const float c=cosine[dir],s=sine[dir];
    const float lu=(((packedLight>>4)&15)+.5f)/16.f;
    const float lv=(((packedLight>>20)&15)+.5f)/16.f;
    std::array<Vertex,4> vertices;
    for(int i=0;i<4;++i){
        const auto p=points[i];
        vertices[i]={center.x+p.x*c+p.z*s,center.y+p.y,center.z-p.x*s+p.z*c,
            uv[i].u,uv[i].v,1,1,1,1,lu,lv};
    }
    for(int i:{0,1,2,0,2,3})mesh.push_back(vertices[i]);
}
Point center(const HangingDecoration& d,int width,int height){
    const float offset=.5f+1.f/16.f;
    const float horizontal=width==32 || width==64?.5f:0.f;
    const float vertical=height==32 || height==64?.5f:0.f;
    Point p{d.tileX+.5f,d.tileY+.5f+vertical,d.tileZ+.5f};
    if(d.dir==0){p.z+=offset;p.x+=horizontal;}
    if(d.dir==1){p.x-=offset;p.z+=horizontal;}
    if(d.dir==2){p.z-=offset;p.x-=horizontal;}
    if(d.dir==3){p.x+=offset;p.z-=horizontal;}
    return p;
}
std::array<Tex,4> tileUV(int tile){
    const float u=(tile%16)/16.f,v=(tile/16)/16.f;
    return {{{u,v},{u+1.f/16,v},{u+1.f/16,v+1.f/16},{u,v+1.f/16}}};
}
void box(std::vector<Vertex>& mesh,Point origin,int dir,int light,float x0,float y0,float z0,
         float x1,float y1,float z1,int tile){
    const auto uv=tileUV(tile);
    quad(mesh,{{{x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0}}},uv,origin,dir,light);
    quad(mesh,{{{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{x0,y0,z1}}},uv,origin,dir,light);
    quad(mesh,{{{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{x0,y0,z0}}},uv,origin,dir,light);
    quad(mesh,{{{x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}}},uv,origin,dir,light);
    quad(mesh,{{{x0,y1,z0},{x0,y1,z1},{x1,y1,z1},{x1,y1,z0}}},uv,origin,dir,light);
    quad(mesh,{{{x0,y0,z1},{x0,y0,z0},{x1,y0,z0},{x1,y0,z1}}},uv,origin,dir,light);
}
}
HangingMesh buildHangingMesh(const HangingDecoration& d,int light,
                            const std::function<int(int,int,int)>& paintingLight){
    HangingMesh result;
    if(d.dir<0 || d.dir>3)return result;
    if(d.kind==HangingDecoration::Kind::Painting){
        // Painting::readAdditionalSaveData falls back to Kebab for an unknown motive.
        const Motive* motive=&motives[0];
        for(const auto& choice:motives)if(d.motive==choice.name){motive=&choice;break;}
        const auto p=center(d,motive->width,motive->height);
        for(int xs=0;xs<motive->width/16;++xs)for(int ys=0;ys<motive->height/16;++ys){
            const float x0=(-motive->width/2.f+(xs+1)*16)/16.f;
            const float x1=(-motive->width/2.f+xs*16)/16.f;
            const float y0=(-motive->height/2.f+(ys+1)*16)/16.f;
            const float y1=(-motive->height/2.f+ys*16)/16.f;
            const float z0=-.5f/16.f,z1=.5f/16.f;
            // PaintingRenderer::setBrightness samples each 16-pixel segment
            // from the wall location behind that segment.
            int segmentLight=light;
            if(paintingLight){
                int lx=int(std::floor(p.x)),ly=int(std::floor(p.y+(y0+y1)*.5f)),lz=int(std::floor(p.z));
                const float sideways=(x0+x1)*.5f;
                if(d.dir==0)lx=int(std::floor(p.x+sideways));
                if(d.dir==1)lz=int(std::floor(p.z-sideways));
                if(d.dir==2)lx=int(std::floor(p.x-sideways));
                if(d.dir==3)lz=int(std::floor(p.z+sideways));
                segmentLight=paintingLight(lx,ly,lz);
            }
            const float fu0=(motive->u+motive->width-xs*16)/256.f;
            const float fu1=(motive->u+motive->width-(xs+1)*16)/256.f;
            const float fv0=(motive->v+motive->height-ys*16)/256.f;
            const float fv1=(motive->v+motive->height-(ys+1)*16)/256.f;
            const float bu0=192.f/256,bu1=208.f/256,bv0=0,bv1=16.f/256;
            const float edgeU=192.5f/256,edgeV=.5f/256;
            quad(result.painting,{{{x0,y1,z0},{x1,y1,z0},{x1,y0,z0},{x0,y0,z0}}},
                {{{fu1,fv0},{fu0,fv0},{fu0,fv1},{fu1,fv1}}},p,d.dir,segmentLight);
            quad(result.painting,{{{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}}},
                {{{bu0,bv0},{bu1,bv0},{bu1,bv1},{bu0,bv1}}},p,d.dir,segmentLight);
            quad(result.painting,{{{x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}}},
                {{{bu0,edgeV},{bu1,edgeV},{bu1,edgeV},{bu0,edgeV}}},p,d.dir,segmentLight);
            quad(result.painting,{{{x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0}}},
                {{{bu0,edgeV},{bu1,edgeV},{bu1,edgeV},{bu0,edgeV}}},p,d.dir,segmentLight);
            quad(result.painting,{{{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{x0,y0,z0}}},
                {{{edgeU,bv0},{edgeU,bv1},{edgeU,bv1},{edgeU,bv0}}},p,d.dir,segmentLight);
            quad(result.painting,{{{x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}}},
                {{{edgeU,bv0},{edgeU,bv1},{edgeU,bv1},{edgeU,bv0}}},p,d.dir,segmentLight);
        }
        return result;
    }
    const auto p=center(d,12,12);
    // ItemFrameRenderer::drawFrame: 12x12 wood rim and ten-pixel inset back.
    constexpr float outer=6.f/16,inner=5.f/16,depth=1.f/16;
    box(result.frame,p,d.dir,light,-inner,-inner,0,inner,inner,depth*.5f,9+11*16);
    const int birchTop=textureTile(Log,1,2);
    box(result.frame,p,d.dir,light,-outer,-outer,0,outer,-inner,depth,birchTop);
    box(result.frame,p,d.dir,light,-outer,inner,0,outer,outer,depth,birchTop);
    box(result.frame,p,d.dir,light,-outer,-inner,0,-inner,inner,depth,birchTop);
    box(result.frame,p,d.dir,light,inner,-inner,0,outer,inner,depth,birchTop);
    if(d.itemId>0){
        const bool block=d.itemId<256;
        const int tile=block?textureTile(static_cast<Block>(d.itemId),3,d.itemDamage):
            consoleItemIcon(d.itemId,d.itemDamage).tile;
        if(tile>=0){
            auto& mesh=block?result.itemTerrain:result.itemAtlas;
            constexpr float half=.23f;
            std::array<Point,4> corners{{{-half,-half,-.004f},{half,-half,-.004f},
                                         {half,half,-.004f},{-half,half,-.004f}}};
            const float angle=-d.itemRotation*3.14159265358979323846f/2;
            const float c=std::cos(angle),s=std::sin(angle);
            for(auto& point:corners){const float x=point.x,y=point.y;point.x=x*c-y*s;point.y=x*s+y*c;}
            quad(mesh,corners,tileUV(tile),p,d.dir,light);
        }
    }
    return result;
}
}
