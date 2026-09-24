#pragma once
#include "RenderLightAccess.h"
#include <algorithm>
#include <cstdlib>
struct RenderLightFixture:console::RenderLightAccess {
    bool ceiling=false,propagate=false,missing=false;
    bool hasCeiling()const override{return ceiling;}
    bool hasChunk(int x,int z)const override{return !missing && x>=-2 && x<2 && z>=-2 && z<2;}
    int tile(int,int,int)const override{return 1;}
    bool propagates(int)const override{return propagate;}
    int storedLight(LightLayer::variety layer,int x,int y,int z)const override {
        if(y<0 || y>=256)return 0;
        return int((std::uint32_t(x)*3+std::uint32_t(y)*7+std::uint32_t(z)*11+(layer==LightLayer::Sky?5:0))&15);
    }
    int brightness(LightLayer::variety layer,int x,int y,int z)const override {
        if(!hasChunk(int(std::floor(x/16.0)),int(std::floor(z/16.0))))return int(layer);
        return storedLight(layer,x,std::clamp(y,0,255),z);
    }
};
