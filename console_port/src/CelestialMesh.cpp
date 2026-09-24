#include "CelestialMesh.h"
#include "ConsoleLightmap.h"
#include <bit>
#include <cmath>
#include <stdexcept>
namespace console {
std::vector<Vertex> buildCloudMesh(Vec3 eye,std::int32_t animationTicks,const std::array<float,3>& colour,float partialTick){
    if(!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z) || std::abs(eye.x)>30000000 || std::abs(eye.y)>30000000 || std::abs(eye.z)>30000000 || !std::isfinite(partialTick) || partialTick<0 || partialTick>1)
        throw std::invalid_argument("Invalid cloud mesh input");
    for(float channel:colour)if(!std::isfinite(channel) || channel<0 || channel>1)throw std::invalid_argument("Invalid cloud tint");
    // LevelRenderer::renderClouds basic path: 32-block patches, 2048-block repeat.
    float scale=1/2048.f;
    double time=animationTicks+partialTick;
    double xo=eye.x+time*.03f,zo=eye.z;
    xo-=std::floor(xo/2048)*2048;zo-=std::floor(zo/2048)*2048;
    float uo=static_cast<float>(xo*scale),vo=static_cast<float>(zo*scale);
    auto vertex=[&](int x,int z){return Vertex{float(eye.x+x),128+.33f,float(eye.z+z),x*scale+uo,z*scale+vo,colour[0],colour[1],colour[2],.8f};};
    std::vector<Vertex> result;result.reserve(16*16*6);
    for(int x=-256;x<256;x+=32)for(int z=-256;z<256;z+=32){
        std::array<Vertex,4> corners{vertex(x,z+32),vertex(x+32,z+32),vertex(x+32,z),vertex(x,z)};
        for(int i:{0,1,2,0,2,3})result.push_back(corners[i]);
    }
    return result;
}
int consoleMoonPhase(std::int64_t time){
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(time/24000))%8;
}
CelestialMesh buildCelestialMesh(std::int64_t time,float rain,float partialTick){
    if(!std::isfinite(rain) || rain<0 || rain>1)throw std::invalid_argument("Invalid celestial rain level");
    float angle=consoleTimeOfDay(time,partialTick)*2*3.14159265358979323846f;
    float sine=std::sin(angle),cosine=std::cos(angle);
    auto vertex=[&](float x,float y,float z,float u,float v){
        // Original glRotate(-90,Y), followed by glRotate(timeOfDay*360,X).
        return Vertex{-(y*sine+z*cosine),y*cosine-z*sine,x,u,v,1,1,1,1-rain};
    };
    auto triangles=[](std::array<Vertex,4> c){return std::vector<Vertex>{c[0],c[1],c[2],c[0],c[2],c[3]};};
    CelestialMesh result;
    result.sun=triangles({vertex(-30,100,-30,0,0),vertex(30,100,-30,1,0),vertex(30,100,30,1,1),vertex(-30,100,30,0,1)});
    int phase=consoleMoonPhase(time),u=phase%4,v=phase/4%2;
    float u0=u/4.f,v0=v/2.f,u1=(u+1)/4.f,v1=(v+1)/2.f;
    result.moon=triangles({vertex(-20,-100,20,u1,v1),vertex(20,-100,20,u0,v1),vertex(20,-100,-20,u0,v0),vertex(-20,-100,-20,u1,v0)});
    return result;
}
}
