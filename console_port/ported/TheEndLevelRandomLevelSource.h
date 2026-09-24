#pragma once
#include "WorldGenLevel.h"
#include "PerlinNoise.h"
// Original End island stages. Decoration, crystals and dragon spawning remain
// separate from the terrain arrays produced by this source.
class TheEndLevelRandomLevelSource {
    int m_XZSize=0;
    Level* level=nullptr;
    std::unique_ptr<Random> random,pprandom;
    std::unique_ptr<PerlinNoise> lperlinNoise1,lperlinNoise2,perlinNoise1,scaleNoise,depthNoise;
    doubleArray getHeights(doubleArray,int,int,int,int,int,int);
public:
    static constexpr int CHUNK_HEIGHT=4,CHUNK_WIDTH=8;
    TheEndLevelRandomLevelSource(Level*,std::int64_t);
    void seedChunk(int x,int z){random->setSeed(x*341873128712LL+z*132897987541LL);}
    void prepareHeights(int,int,byteArray,BiomeArray);
    void buildSurfaces(int,int,byteArray,BiomeArray);
};
