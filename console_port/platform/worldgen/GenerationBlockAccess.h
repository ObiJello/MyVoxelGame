#pragma once
#include "LightLayer.h"
class Level;
// Explicit block-data dependency for original placement features. Simulation
// notifications remain outside generation, which writes without updating neighbors.
class GenerationBlockAccess {
public:
    virtual ~GenerationBlockAccess()=default;
    virtual int getTile(int x,int y,int z) const=0;
    virtual int getData(int x,int y,int z) const=0;
    virtual bool setTileAndData(int x,int y,int z,int tile,int data)=0;
    virtual int getHeightmap(int x,int z) const=0;
    virtual int getDaytimeRawBrightness(int x,int y,int z)const=0;
    virtual bool hasChunk(int x,int z)const=0;
    virtual int getLight(LightLayer::variety,int x,int y,int z)const=0;
    virtual void setLight(LightLayer::variety,int x,int y,int z,int value)=0;
    virtual bool hasPreparedLight()const=0;
    virtual void acceptPreparedLight()=0;
    virtual void updateColumnLight(Level&,int x,int y,int z,int oldHeight)=0;
    virtual void lightColumnChanged(int x,int z)=0;
};
