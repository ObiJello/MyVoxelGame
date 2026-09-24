#include "stdafx.h"
#include "WorldGenLevel.h"
#include "GenerationRegion.h"
#include <iostream>
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
static std::uint64_t fingerprint(const console::GenerationRegion& region){
    std::uint64_t h=14695981039346656037ULL;
    for(const auto& [position,chunk]:region.chunks){
        for(auto v:chunk->heightmap)h=(h^v)*1099511628211ULL;
        for(unsigned i=0;i<chunk->skyLight.data.length;++i)h=(h^chunk->skyLight.data[i])*1099511628211ULL;
        for(unsigned i=0;i<chunk->blockLight.data.length;++i)h=(h^chunk->blockLight.data[i])*1099511628211ULL;
    }
    return h;
}
int main(){try{
    for(int origin:{-1,0,15}){
        console::GenerationRegion region;
        for(int cx=-2;cx<=2;++cx)for(int cz=-2;cz<=1;++cz){
            auto chunk=std::make_unique<console::ChunkStorage>();
            for(int column=0;column<256;++column)for(int y=0;y<=63;++y)chunk->blocks[column*256+y]=y==63?2:1;
            chunk->recalculateHeightmap();region.insert(cx,cz,std::move(chunk));
        }
        Level level(1);level.setBlockAccess(region);
        for(int dx=-2;dx<=2;++dx)for(int dz=-2;dz<=2;++dz)for(int y=29;y<=33;++y)
            level.setTileNoUpdateNoLightCheck(origin+dx,y,dz,0);
        level.setTileNoUpdateNoLightCheck(origin-1,31,0,89);
        level.setTileNoUpdateNoLightCheck(origin,80,0,1);
        region.initializeLight(level);
        require(level.getBrightness(LightLayer::Sky,origin,81,0)==15,"Sky above isolated roof is not full brightness");
        require(level.getBrightness(LightLayer::Sky,origin,79,0)==14,"Initial gap checks did not light below roof from the sides");
        require(level.getBrightness(LightLayer::Sky,origin,31,0)==0,"Sealed chamber received skylight");
        require(level.getBrightness(LightLayer::Block,origin,31,0)==14,"Initial emissive scan did not light chamber");
        std::cout<<origin<<" initial "<<fingerprint(region)<<'\n';
        level.setTileNoUpdate(origin,80,0,0);
        require(region.hasPreparedLight() && level.getHeightmap(origin,0)==64,"Removing roof invalidated lighting or left old height");
        require(level.getBrightness(LightLayer::Sky,origin,79,0)==15,"Roof removal left shadow in its column");
        require(region.changedLightColumns.contains({origin,0}),"Light changes did not mark the column");
        std::cout<<origin<<" removed "<<fingerprint(region)<<'\n';
        level.setTileNoUpdate(origin,90,0,9);
        require(level.getHeightmap(origin,0)==91 && level.getBrightness(LightLayer::Sky,origin,90,0)==12,"New water canopy has incorrect height or attenuation");
        require(level.getBrightness(LightLayer::Sky,origin,89,0)==14,"Side lighting did not repair shadow under new water");
        std::cout<<origin<<" water "<<fingerprint(region)<<'\n';
        level.setTileNoUpdate(origin-1,31,0,0);
        require(level.getBrightness(LightLayer::Block,origin,31,0)==0,"Chamber stayed lit after source removal");
        std::cout<<origin<<" dark "<<fingerprint(region)<<'\n';
    }
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
