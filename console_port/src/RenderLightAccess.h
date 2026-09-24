#pragma once
#include "LightLayer.h"
namespace console {
class RenderLightAccess {
public:
    virtual ~RenderLightAccess()=default;
    virtual bool hasCeiling() const=0;
    virtual bool hasChunk(int x,int z) const=0;
    virtual int tile(int x,int y,int z) const=0;
    virtual bool propagates(int tile) const=0;
    // Original Level::getBrightness: neighbor queries clamp vertical coordinates.
    virtual int brightness(LightLayer::variety layer,int x,int y,int z) const=0;
    // Original chunk query; host storage rejects or returns zero outside height.
    virtual int storedLight(LightLayer::variety layer,int x,int y,int z) const=0;
};
int sampleConsoleLight(const RenderLightAccess& access,int x,int y,int z,int emission,int tileId=-1);
}
