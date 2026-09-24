#include "ExperienceOrbMesh.h"
#include "ExperienceOrbRules.h"
#include <array>
#include <algorithm>
#include <cmath>
namespace console {
std::vector<Vertex> buildExperienceOrbMesh(const ExperienceOrbState& orb,
    double yaw,double pitch,int packedLight){
    const int icon=sourceExperienceOrbIcon(orb.value);
    const float u0=float(icon%4)*.25f,u1=u0+.25f;
    const float v0=float(icon/4)*.25f,v1=v0+.25f;
    const float rightX=std::cos(yaw),rightZ=std::sin(yaw);
    const float upX=-std::sin(yaw)*std::sin(pitch),
                upY=std::cos(pitch),upZ=std::cos(yaw)*std::sin(pitch);
    const float phase=float(orb.age)*.5f;
    const float red=(std::sin(phase)+1.f)*.5f;
    const float blue=(std::sin(phase+2.f*3.14159265358979323846f/3.f)+1.f)*.1f;
    const int block=std::min(240,(packedLight&0xff)+120);
    const float lu=(float((block>>4)&15)+.5f)/16.f;
    const float lv=(float((packedLight>>20)&15)+.5f)/16.f;
    // The source renderer scales its one-unit billboard by .3 and places it
    // from y=-.25 to y=.75 around the entity position.
    const std::array<std::array<float,4>,4> corners{{
        {{-.15f,-.075f,u0,v1}},{{.15f,-.075f,u1,v1}},
        {{.15f,.225f,u1,v0}},{{-.15f,.225f,u0,v0}}
    }};
    std::array<Vertex,4> quad;
    for(int i=0;i<4;++i){
        const auto& c=corners[i];
        quad[i]={float(orb.position.x)+rightX*c[0]+upX*c[1],
                 float(orb.position.y)+upY*c[1],
                 float(orb.position.z)+rightZ*c[0]+upZ*c[1],
                 c[2],c[3],red,1.f,blue,.5f,lu,lv};
    }
    return {quad[0],quad[1],quad[2],quad[0],quad[2],quad[3]};
}
}
