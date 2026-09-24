#pragma once
#include "WorldGenLevel.h"
#include "PerlinNoise.h"
#include "net.minecraft.world.level.tile.h"
#include "GenerationOptions.h"

// Original density/surface stages. Structures and postProcess are a later boundary.
class RandomLevelSource {
    int m_XZSize;
    Level* level;
    Random* random;
    PerlinNoise *lperlinNoise1,*lperlinNoise2,*perlinNoise1,*perlinNoise3;
    PerlinNoise *scaleNoise,*depthNoise,*floatingIslandScale,*floatingIslandNoise,*forestNoise;
    floatArray pows;
    doubleArray getHeights(doubleArray,int,int,int,int,int,int,BiomeArray&);
public:
    static constexpr bool FLOATING_ISLANDS=false;
    static constexpr int CHUNK_HEIGHT=8,CHUNK_WIDTH=4;
    explicit RandomLevelSource(Level* world,std::int64_t seed);
    ~RandomLevelSource();
    RandomLevelSource(const RandomLevelSource&)=delete;
    RandomLevelSource& operator=(const RandomLevelSource&)=delete;
    void seedChunk(int x,int z){random->setSeed(x*341873128712LL+z*132897987541LL);}
    void prepareHeights(int x,int z,byteArray blocks);
    void buildSurfaces(int x,int z,byteArray blocks,BiomeArray biomes);
};
