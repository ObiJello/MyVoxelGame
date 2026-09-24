#pragma once
#include "LiquidSurface.h"
#include <array>
namespace console {
class LiquidFlowAccess:public LiquidSurfaceAccess {
public:
    virtual bool blocksMotion(int x,int y,int z)const=0;
    virtual bool ice(int x,int y,int z)const=0;
};
struct LiquidFlow { double x,y,z; };
struct LiquidUV { float u,v; };
LiquidFlow consoleLiquidFlow(const LiquidFlowAccess& access,int x,int y,int z);
double consoleLiquidSlope(const LiquidFlowAccess& access,int x,int y,int z);
// Corner order: northwest, southwest, southeast, northeast.
std::array<LiquidUV,4> consoleLiquidTopUV(float angle,bool lava);
LiquidUV consoleLiquidSideUV(float height,bool right,bool lava);
}
