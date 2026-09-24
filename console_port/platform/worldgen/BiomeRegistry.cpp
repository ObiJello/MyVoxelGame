#include "Biome.h"
#include <array>
namespace {
std::array<Biome,23> registry{{
    {0,-1,0.4f,.5f,.5f,2,3}, // ocean
    {1,.1f,.3f,0.8f,0.4f,2,3}, // plains
    {2,0.1f,0.2f,2,0,12,12}, // desert
    {3,0.3f,1.5f,0.2f,0.3f,2,3}, // extremeHills
    {4,.1f,.3f,0.7f,0.8f,2,3}, // forest
    {5,0.1f,0.4f,0.05f,0.8f,2,3}, // taiga
    {6,-0.2f,0.1f,0.8f,0.9f,2,3}, // swampland
    {7,-0.5f,0,.5f,.5f,2,3}, // river
    {8,.1f,.3f,2,0,2,3}, // hell
    {9,.1f,.3f,.5f,.5f,2,3}, // sky
    {10,-1,0.5f,0,0.5f,2,3}, // frozenOcean
    {11,-0.5f,0,0,0.5f,2,3}, // frozenRiver
    {12,.1f,.3f,0,0.5f,2,3}, // iceFlats
    {13,0.3f,1.3f,0,0.5f,2,3}, // iceMountains
    {14,0.2f,1.0f,0.9f,1.0f,110,3}, // mushroomIsland
    {15,-1,0.1f,0.9f,1.0f,110,3}, // mushroomIslandShore
    {16,0.0f,0.1f,0.8f,0.4f,12,12}, // beaches
    {17,0.3f,0.8f,2,0,12,12}, // desertHills
    {18,0.3f,0.7f,0.7f,0.8f,2,3}, // forestHills
    {19,0.3f,0.8f,0.05f,0.8f,2,3}, // taigaHills
    {20,0.2f,0.8f,0.2f,0.3f,2,3}, // smallerExtremeHills
    {21,0.2f,0.4f,1.2f,0.9f,2,3}, // jungle
    {22,1.8f,0.5f,1.2f,0.9f,2,3}, // jungleHills
}};
}
Biome* Biome::biomes[256]{};
Biome* Biome::ocean=&registry[0];
Biome* Biome::plains=&registry[1];
Biome* Biome::desert=&registry[2];
Biome* Biome::extremeHills=&registry[3];
Biome* Biome::forest=&registry[4];
Biome* Biome::taiga=&registry[5];
Biome* Biome::swampland=&registry[6];
Biome* Biome::river=&registry[7];
Biome* Biome::hell=&registry[8];
Biome* Biome::sky=&registry[9];
Biome* Biome::frozenOcean=&registry[10];
Biome* Biome::frozenRiver=&registry[11];
Biome* Biome::iceFlats=&registry[12];
Biome* Biome::iceMountains=&registry[13];
Biome* Biome::mushroomIsland=&registry[14];
Biome* Biome::mushroomIslandShore=&registry[15];
Biome* Biome::beaches=&registry[16];
Biome* Biome::desertHills=&registry[17];
Biome* Biome::forestHills=&registry[18];
Biome* Biome::taigaHills=&registry[19];
Biome* Biome::smallerExtremeHills=&registry[20];
Biome* Biome::jungle=&registry[21];
Biome* Biome::jungleHills=&registry[22];
namespace { const bool registered=[] { for(auto& biome:registry)Biome::biomes[biome.id]=&biome;return true; }(); }
