#include "stdafx.h"
#include "RandomLevelSource.h"
RandomLevelSource::RandomLevelSource(Level* world,std::int64_t seed):m_XZSize(world->getLevelData()->getXZSize()),level(world){
    // Preserve the original constructor's exact noise allocation/random consumption order.
    random=new Random(seed);
    lperlinNoise1=new PerlinNoise(random,16);
    lperlinNoise2=new PerlinNoise(random,16);
    perlinNoise1=new PerlinNoise(random,8);
    perlinNoise3=new PerlinNoise(random,4);
    scaleNoise=new PerlinNoise(random,10);
    depthNoise=new PerlinNoise(random,16);
    floatingIslandScale=nullptr;floatingIslandNoise=nullptr;
    forestNoise=new PerlinNoise(random,8);
}
RandomLevelSource::~RandomLevelSource(){
    delete random;delete lperlinNoise1;delete lperlinNoise2;delete perlinNoise1;delete perlinNoise3;
    delete scaleNoise;delete depthNoise;delete floatingIslandScale;delete floatingIslandNoise;delete forestNoise;
    delete[] pows.data;
}
