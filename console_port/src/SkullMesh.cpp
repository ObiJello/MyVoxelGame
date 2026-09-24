#include "SkullMesh.h"
#include <array>
#include <cmath>

namespace console {
std::vector<Vertex> buildSkullMesh(int x,int y,int z,int face,int rotation,int light){
    if(face<1 || face>5)return {};
    float cx=x+.5f,cy=float(y),cz=z+.5f;
    float degrees=(rotation&15)*22.5f;
    if(face!=1){
        cy+=.25f;
        if(face==2)cz=z+.74f;
        else if(face==3){cz=z+.26f;degrees=180;}
        else if(face==4){cx=x+.74f;degrees=270;}
        else {cx=x+.26f;degrees=90;}
    }
    const float rad=degrees*3.14159265358979323846f/180.f,c=std::cos(rad),s=std::sin(rad);
    // SkeletonHeadModel::head.addBox(-4,-8,-4,8,8,8,0.1).
    constexpr float x0=-4.1f,x1=4.1f,y0=-8.1f,y1=.1f,z0=-4.1f,z1=4.1f;
    const std::array<std::array<float,3>,8> corners{{
        {{x0,y0,z0}},{{x1,y0,z0}},{{x1,y1,z0}},{{x0,y1,z0}},
        {{x0,y0,z1}},{{x1,y0,z1}},{{x1,y1,z1}},{{x0,y1,z1}}
    }};
    struct Face{std::array<int,4> points;int u0,v0,u1,v1;};
    const std::array<Face,6> faces{{
        {{{5,1,2,6}},16,8,24,16},{{{0,4,7,3}},0,8,8,16},
        {{{5,4,0,1}},8,0,16,8},{{{2,3,7,6}},16,8,32,0},
        {{{1,0,3,2}},8,8,16,16},{{{4,5,6,7}},24,8,32,16}
    }};
    const float lu=(((light>>4)&15)+.5f)/16.f,lv=(((light>>20)&15)+.5f)/16.f;
    std::vector<Vertex> mesh;mesh.reserve(36);
    for(const auto& f:faces){
        const float insetU=f.u1>f.u0?.1f:-.1f,insetV=f.v1>f.v0?.1f:-.1f;
        const float us[]{(f.u1-insetU)/64,(f.u0+insetU)/64,(f.u0+insetU)/64,(f.u1-insetU)/64};
        const float vs[]{(f.v0+insetV)/32,(f.v0+insetV)/32,(f.v1-insetV)/32,(f.v1-insetV)/32};
        std::array<Vertex,4> quad;
        for(int i=0;i<4;++i){
            const auto& point=corners[f.points[i]];
            const float modelX=point[0]*c+point[2]*s,modelZ=-point[0]*s+point[2]*c;
            quad[i]={cx-modelX/16,cy-point[1]/16,cz+modelZ/16,
                     us[i],vs[i],1,1,1,1,lu,lv};
        }
        for(int i:{0,1,2,0,2,3})mesh.push_back(quad[i]);
    }
    return mesh;
}
}
