#include "WorldGenLevel.h"

// LevelChunk::getTopRainBlock and Level::shouldSnow from the supplied console
// source, adapted to the detached generation region. The original caches rain
// heights per chunk; this stage asks each column once, so a scan is sufficient.
int Level::getTopRainBlock(int x,int z){
    for(int y=maxBuildHeight-1;y>0;--y){
        const int id=getTile(x,y,z);
        Material* material=Tile::materialFor(id);
        if(material->blocksMotion() || material->isLiquid())return y+1;
    }
    return -1;
}

bool Level::shouldSnow(int x,int y,int z){
    if(getBiome(x,z)->getTemperature()>0.15f || y<0 || y>=maxBuildHeight ||
       getBrightness(LightLayer::Block,x,y,z)>=10 || getTile(x,y,z)!=0)return false;
    const int below=getTile(x,y-1,z);
    return below!=0 && below!=Tile::ice_Id &&
        (below==Tile::leaves_Id || Tile::solid[below]) &&
        Tile::materialFor(below)->blocksMotion();
}
