#include "stdafx.h"
#include "GenerationRegion.h"
#include "WorldGenLevel.h"
#include <iostream>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::uint64_t hash(const console::GenerationRegion& region){
    std::uint64_t h=14695981039346656037ULL;
    for(const auto& [position,chunk]:region.chunks){
        for(unsigned i=0;i<chunk->skyLight.data.length;++i)h=(h^chunk->skyLight.data[i])*1099511628211ULL;
        for(unsigned i=0;i<chunk->blockLight.data.length;++i)h=(h^chunk->blockLight.data[i])*1099511628211ULL;
    }
    return h;
}
int main(){try{
    for(int origin:{-16,0,15}){
        console::GenerationRegion region;
        for(int cx=-3;cx<=2;++cx)for(int cz=-2;cz<=1;++cz)region.insert(cx,cz,std::make_unique<console::ChunkStorage>());
        Level level(1);level.setBlockAccess(region);
        auto sample=[&](const char* stage){std::cout<<origin<<' '<<stage<<' '<<hash(region)<<'\n';};
        auto brightness=[&](int dx,int dy=0,int dz=0){return level.getBrightness(LightLayer::Block,origin+dx,64+dy,dz);};
        level.setTileNoUpdateNoLightCheck(origin,64,0,10);
        level.checkLight(LightLayer::Block,origin,64,0);
        require(brightness(0)==15 && brightness(1)==14 && brightness(3,2,1)==9,"Emitted light did not attenuate by Manhattan distance");
        sample("lava");
        level.setTileNoUpdateNoLightCheck(origin+1,64,0,1);
        level.checkLight(LightLayer::Block,origin+1,64,0);
        require(brightness(1)==0 && brightness(2)==11,"Opaque block did not block and reroute light");
        sample("blocked");
        level.setTileNoUpdateNoLightCheck(origin+1,64,0,9);
        level.checkLight(LightLayer::Block,origin+1,64,0);
        require(brightness(1)==12,"Water did not attenuate light by three");
        sample("water");
        level.setTileNoUpdateNoLightCheck(origin+6,64,0,89);
        level.checkLight(LightLayer::Block,origin+6,64,0);
        level.setTileNoUpdateNoLightCheck(origin,64,0,0);
        level.checkLight(LightLayer::Block,origin,64,0);
        require(brightness(6)==15 && brightness(5)==14,"Removing one source destroyed the other source's light");
        sample("overlap");
        level.setTileNoUpdateNoLightCheck(origin+6,64,0,0);
        level.checkLight(LightLayer::Block,origin+6,64,0);
        require(brightness(0)==0 && brightness(6)==0 && brightness(3,2,1)==0,"Removed light source left stale illumination");
        sample("dark");
        region.acceptPreparedLight();
        level.setTileNoUpdate(origin,64,0,39);
        require(brightness(0)==1 && brightness(1)==0 && region.hasPreparedLight(),"Brown mushroom emission did not update prepared light");
        level.setTileNoUpdate(origin,64,0,0);
        require(brightness(0)==0 && region.hasPreparedLight(),"Brown mushroom removal left light or invalidated prepared state");
        sample("mushroom");
    }
    // A stone volume with a narrow shaft isolates vertical skylight attenuation.
    console::GenerationRegion region;
    for(int cx=-2;cx<=1;++cx)for(int cz=-2;cz<=1;++cz){
        auto chunk=std::make_unique<console::ChunkStorage>();
        for(int column=0;column<256;++column)for(int y=0;y<=70;++y)chunk->blocks[column*256+y]=1;
        chunk->recalculateHeightmap();region.insert(cx,cz,std::move(chunk));
    }
    Level level(1);level.setBlockAccess(region);
    for(int y=60;y<=70;++y)level.setTileNoUpdateNoLightCheck(0,y,0,0);
    level.setTileNoUpdateNoLightCheck(0,65,0,9);
    for(int y=66;y<=71;++y)region.setLight(LightLayer::Sky,0,y,0,15);
    level.checkLight(LightLayer::Sky,0,65,0);
    require(level.getBrightness(LightLayer::Sky,0,65,0)==12 && level.getBrightness(LightLayer::Sky,0,64,0)==11,"Skylight did not propagate below water");
    std::cout<<"sky "<<hash(region)<<'\n';
    // Standard updates wait for all neighbors; forced updates can seed one chunk.
    console::GenerationRegion small;small.insert(0,0,std::make_unique<console::ChunkStorage>());
    Level partial(1);partial.setBlockAccess(small);partial.setTileNoUpdateNoLightCheck(7,64,7,89);
    partial.checkLight(LightLayer::Block,7,64,7);
    require(partial.getBrightness(LightLayer::Block,7,64,7)==0,"Normal light update ignored missing neighbors");
    partial.checkLight(LightLayer::Block,7,64,7,true,true);
    require(partial.getBrightness(LightLayer::Block,7,64,7)==15,"Forced emission did not light the prepared chunk");
    std::cout<<"forced "<<hash(small)<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
