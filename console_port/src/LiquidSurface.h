#pragma once
#include "RenderLightAccess.h"
namespace console {
class LiquidSurfaceAccess {
public:
    virtual ~LiquidSurfaceAccess()=default;
    virtual bool sameLiquid(int x,int y,int z)const=0;
    virtual bool solidMaterial(int x,int y,int z)const=0;
    virtual int data(int x,int y,int z)const=0;
};
float consoleLiquidDepth(int data);
float consoleLiquidCorner(const LiquidSurfaceAccess& access,int x,int y,int z);
int sampleConsoleLiquidLight(const RenderLightAccess& access,int x,int y,int z,int tileId=-1);
}
