#pragma once
#include "WorldGenLevel.h"
#include "PerlinNoise.h"
// Original Nether density and surface stages; fortress generation and postProcess
// are separate boundaries. Owned noise objects preserve original allocation order.
class HellRandomLevelSource {
    int m_XZSize=0;
    Level* level=nullptr;
    std::unique_ptr<Random> random,pprandom;
    std::unique_ptr<PerlinNoise> lperlinNoise1,lperlinNoise2,perlinNoise1,perlinNoise2,perlinNoise3,scaleNoise,depthNoise;
    doubleArray getHeights(doubleArray,int,int,int,int,int,int);
public:
    static constexpr int CHUNK_HEIGHT=8,CHUNK_WIDTH=4;
    HellRandomLevelSource(Level*,std::int64_t);
    void seedChunk(int x,int z){random->setSeed(x*341873128712LL+z*132897987541LL);}
    void prepareHeights(int,int,byteArray);
    void buildSurfaces(int,int,byteArray);
};
