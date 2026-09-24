#pragma once
class Random;
namespace console {
// LiquidTileDynamic::mainTick's block rules. The host supplies bounded world
// access and owns scheduling, lighting, persistence and neighbor notifications.
class FlowingFluidAccess {
public:
    virtual ~FlowingFluidAccess()=default;
    virtual bool inside(int x,int y,int z)const=0;
    virtual int tile(int x,int y,int z)const=0;
    virtual int data(int x,int y,int z)const=0;
    virtual bool blocksMotion(int x,int y,int z)const=0;
    virtual void put(int x,int y,int z,int id,int data)=0;
};
void tickFlowingFluid(FlowingFluidAccess& level,Random& random,int x,int y,int z);
}
