#include "stdafx.h"
#include "GenerationRegion.h"
#include "WorldGenLevel.h"
#include "LakeFeature.h"
#include "IntCache.h"
#include <iostream>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::uint64_t fingerprint(const console::GenerationRegion& region){
    std::uint64_t h=14695981039346656037ULL;
    for(const auto& [position,chunk]:region.chunks){
        for(auto b:chunk->blocks)h=(h^b)*1099511628211ULL;
        for(auto b:chunk->heightmap)h=(h^b)*1099511628211ULL;
        for(unsigned i=0;i<chunk->metadata.data.length;++i){
            h=(h^chunk->metadata.data[i])*1099511628211ULL;
            h=(h^chunk->skyLight.data[i])*1099511628211ULL;
            h=(h^chunk->blockLight.data[i])*1099511628211ULL;
        }
    }
    return h;
}
int main(){try{
    IntCache::CreateNewThreadStorage();
    for(auto seed:{0LL,8675309LL})for(int liquid:{9,11})for(int flooded:{0,1}){
        console::GenerationRegion region;
        for(int cx=-2;cx<=1;++cx)for(int cz=-2;cz<=1;++cz){
            auto chunk=std::make_unique<console::ChunkStorage>();
            for(int column=0;column<256;++column){
                for(int y=0;y<=63;++y)chunk->blocks[column*256+y]=y==63?2:y>=58?3:1;
                if(flooded)for(int y=64;y<=68;++y)chunk->blocks[column*256+y]=9;
            }
            chunk->recalculateHeightmap();region.insert(cx,cz,std::move(chunk));
        }
        Level level(seed);level.setBlockAccess(region);region.initializeLight(level);
        Random random(seed);LakeFeature lake(liquid);auto before=fingerprint(region);
        bool placed=lake.place(&level,&random,0,70,0);auto after=fingerprint(region);
        if(!flooded)require(placed && before!=after,"Lake failed to carve valid terrain");
        else require(!placed && before==after,"Lake carved through an existing liquid boundary");
        require(region.hasPreparedLight(),"Lake placement left stale light");
        std::cout<<seed<<' '<<liquid<<' '<<flooded<<' '<<placed<<' '<<after<<' '<<random.nextLong()<<'\n';
    }
    std::unique_ptr<Level> cold;
    for(int seed=0;seed<256 && !cold;++seed){
        auto candidate=std::make_unique<Level>(seed);
        if(candidate->getBiome(0,0)->getTemperature()<=0.15f)cold=std::move(candidate);
    }
    require(bool(cold),"Cold-biome test fixture was not found");
    console::GenerationRegion region;region.insert(-1,0,std::make_unique<console::ChunkStorage>());
    region.insert(0,-1,std::make_unique<console::ChunkStorage>());region.insert(0,0,std::make_unique<console::ChunkStorage>());
    cold->setBlockAccess(region);cold->setTileNoUpdateNoLightCheck(0,63,0,9);
    require(cold->shouldFreezeIgnoreNeighbors(0,63,0) && cold->shouldFreeze(0,63,0),"Exposed cold source water did not freeze");
    for(auto p:{std::pair{-1,0},std::pair{1,0},std::pair{0,-1},std::pair{0,1}})cold->setTileNoUpdateNoLightCheck(p.first,63,p.second,9);
    require(!cold->shouldFreeze(0,63,0) && cold->shouldFreezeIgnoreNeighbors(0,63,0),"Water neighbor freezing rule differs");
    region.setLight(LightLayer::Block,0,63,0,10);
    require(!cold->shouldFreezeIgnoreNeighbors(0,63,0),"Bright water froze");
    region.setLight(LightLayer::Block,0,63,0,0);region.setTileAndData(0,63,0,9,1);
    require(!cold->shouldFreezeIgnoreNeighbors(0,63,0),"Non-source water froze");
    std::cout<<"freeze "<<cold->getSeed()<<'\n';
    IntCache::ReleaseThreadStorage();
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
