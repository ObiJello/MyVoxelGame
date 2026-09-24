#pragma once
#include "TerrainMesh.h"
#include "Random.h"
namespace console {
struct BookAnimation {
    Random random;
    std::int64_t lastWorldTick=-1;
    float open=0,oOpen=0,rot=0,oRot=0,tRot=0,flip=0,oFlip=0,flipT=0,flipA=0;
    int time=0;
    explicit BookAnimation(std::int64_t seed):random(seed){}
    void tick(Vec3 player,int x,int y,int z);
};
std::vector<Vertex> buildBookMesh(int x,int y,int z,const BookAnimation& animation,int packedLight,float partialTick=1);
}
