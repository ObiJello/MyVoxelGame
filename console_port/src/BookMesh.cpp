#include "BookMesh.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace console {
namespace {
constexpr float pi=3.14159265358979323846f;
struct Point{float x,y,z;};
struct Part{
    Point origin;float x,y,z;int width,height,depth,u,v,mask;float yaw;
};
void addPart(std::vector<Vertex>& mesh,const Part& part,Point world,float worldYaw,int light){
    const float x0=part.x,x1=part.x+part.width,y0=part.y,y1=part.y+part.height,z0=part.z,z1=part.z+part.depth;
    const std::array<Point,8> corner{{{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
                                      {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}}};
    struct Face{std::array<int,4> points;int u0,v0,u1,v1;};
    const int u=part.u,v=part.v,w=part.width,h=part.height,d=part.depth;
    const std::array<Face,6> faces{{
        {{{5,1,2,6}},u+d+w,v+d,u+d+w+d,v+d+h},
        {{{0,4,7,3}},u,v+d,u+d,v+d+h},
        {{{5,4,0,1}},u+d,v,u+d+w,v+d},
        {{{2,3,7,6}},u+d+w,v+d,u+d+w+w,v},
        {{{1,0,3,2}},u+d,v+d,u+d+w,v+d+h},
        {{{4,5,6,7}},u+d+w+d,v+d,u+d+w+d+w,v+d+h}
    }};
    const float lu=(((light>>4)&15)+.5f)/16.f,lv=(((light>>20)&15)+.5f)/16.f;
    const float c=std::cos(part.yaw),s=std::sin(part.yaw);
    const float zc=std::cos(80*pi/180),zs=std::sin(80*pi/180);
    const float yc=std::cos(-worldYaw),ys=std::sin(-worldYaw);
    for(int face=0;face<6;++face){
        if(!(part.mask&(1<<face)))continue;
        const auto& f=faces[face];
        const float insetU=f.u1>f.u0?.1f:-.1f,insetV=f.v1>f.v0?.1f:-.1f;
        const float us[]{(f.u1-insetU)/64,(f.u0+insetU)/64,(f.u0+insetU)/64,(f.u1-insetU)/64};
        const float vs[]{(f.v0+insetV)/32,(f.v0+insetV)/32,(f.v1-insetV)/32,(f.v1-insetV)/32};
        std::array<Vertex,4> quad;
        for(int i=0;i<4;++i){
            const auto p=corner[f.points[i]];
            const float px=p.x*c+p.z*s+part.origin.x;
            const float py=p.y+part.origin.y;
            const float pz=-p.x*s+p.z*c+part.origin.z;
            const float localX=px*zc-py*zs,localY=px*zs+py*zc;
            const float localWorldX=localX*yc+pz*ys,localWorldZ=-localX*ys+pz*yc;
            quad[i]={world.x+localWorldX/16,world.y+localY/16,world.z+localWorldZ/16,
                     us[i],vs[i],1,1,1,1,lu,lv};
        }
        for(int i:{0,1,2,0,2,3})mesh.push_back(quad[i]);
    }
}
float wrap(float value){while(value>=pi)value-=2*pi;while(value<-pi)value+=2*pi;return value;}
}
void BookAnimation::tick(Vec3 player,int x,int y,int z){
    oOpen=open;oRot=rot;
    const double dx=player.x-(x+.5),dy=player.y-(y+.5),dz=player.z-(z+.5);
    const bool nearby=dx*dx+dy*dy+dz*dz<9;
    if(nearby){
        tRot=std::atan2(float(dz),float(dx));open+=.1f;
        if(open<.5f || random.nextInt(40)==0){
            const float old=flipT;
            do{flipT+=random.nextInt(4)-random.nextInt(4);}while(old==flipT);
        }
    }else{tRot+=.02f;open-=.1f;}
    rot=wrap(rot);tRot=wrap(tRot);
    rot+=wrap(tRot-rot)*.4f;
    open=std::clamp(open,0.f,1.f);
    ++time;oFlip=flip;
    const float diff=std::clamp((flipT-flip)*.4f,-.2f,.2f);
    flipA+=(diff-flipA)*.9f;
    flip+=flipA;
}
std::vector<Vertex> buildBookMesh(int x,int y,int z,const BookAnimation& state,int light,float a){
    a=std::clamp(a,0.f,1.f);
    const float tt=state.time+a;
    const float worldYaw=state.oRot+wrap(state.rot-state.oRot)*a;
    const float page=state.oFlip+(state.flip-state.oFlip)*a;
    auto flipValue=[&](float offset){
        float value=page+offset;
        value=(value-std::floor(value))*1.6f-.3f;
        return std::clamp(value,0.f,1.f);
    };
    const float first=flipValue(.25f),second=flipValue(.75f);
    const float open=state.oOpen+(state.open-state.oOpen)*a;
    const float openness=(std::sin(tt*.02f)*.10f+1.25f)*open;
    const float pageOffset=std::sin(openness);
    // EnchantTableRenderer's two translations plus its 80-degree Z tilt.
    const Point world{x+.5f,y+.75f+.1f+std::sin(tt*.1f)*.01f,z+.5f};
    const std::array<Part,7> parts{{
        {{0,0,-1},-6,-5,0,6,10,0,0,0,63,pi+openness},
        {{0,0,1},0,-5,0,6,10,0,16,0,63,-openness},
        {{0,0,0},-1,-5,0,2,10,0,12,0,63,pi/2},
        {{pageOffset,0,0},0,-4,-.99f,5,8,1,0,10,47,openness},
        {{pageOffset,0,0},0,-4,-.01f,5,8,1,12,10,31,-openness},
        {{pageOffset,0,0},0,-4,0,5,8,0,24,10,63,openness-openness*2*first},
        {{pageOffset,0,0},0,-4,0,5,8,0,24,10,63,openness-openness*2*second}
    }};
    std::vector<Vertex> mesh;mesh.reserve(240);
    for(const auto& part:parts)addPart(mesh,part,world,worldYaw,light);
    return mesh;
}
}
