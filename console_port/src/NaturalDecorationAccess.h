#pragma once
#include "GenerationRegion.h"

namespace console {
// Detached, generation-only block access. Plant survival checks see daytime
// brightness at the surface while live light propagation is deferred until
// the decorated columns are committed to a world region.
class NaturalDecorationAccess final:public GenerationBlockAccess {
    GenerationRegion& region;
public:
    explicit NaturalDecorationAccess(GenerationRegion& value):region(value){}
    int getTile(int x,int y,int z)const override{return region.getTile(x,y,z);}
    int getData(int x,int y,int z)const override{return region.getData(x,y,z);}
    bool setTileAndData(int x,int y,int z,int tile,int data)override{return region.setTileAndData(x,y,z,tile,data);}
    int getHeightmap(int x,int z)const override{return region.getHeightmap(x,z);}
    int getDaytimeRawBrightness(int x,int y,int z)const override{
        if(y<0)return 0;
        return y>=region.getHeightmap(x,z)-1?15:0;
    }
    bool hasChunk(int x,int z)const override{return region.hasChunk(x,z);}
    int getLight(LightLayer::variety layer,int x,int y,int z)const override{
        return layer==LightLayer::Sky?getDaytimeRawBrightness(x,y,z):0;
    }
    void setLight(LightLayer::variety,int,int,int,int)override{}
    bool hasPreparedLight()const override{return false;}
    void acceptPreparedLight()override{}
    void updateColumnLight(Level&,int,int,int,int)override{}
    void lightColumnChanged(int,int)override{}
};
}
