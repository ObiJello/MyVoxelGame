#include "stdafx.h"
#include "GenerationRegion.h"
#include "WorldGenLevel.h"
#include "FlowerFeature.h"
#include "TallGrassFeature.h"
#include "DeadBushFeature.h"
#include "ReedsFeature.h"
#include "CactusFeature.h"
#include "WaterlilyFeature.h"
#include "PumpkinFeature.h"
#include "VinesFeature.h"
#include "IntCache.h"
#include <iostream>

static std::uint64_t hash(const console::GenerationRegion& region){
    std::uint64_t result=14695981039346656037ULL;
    for(const auto& [position,chunk]:region.chunks){
        for(auto value:chunk->blocks)result=(result^value)*1099511628211ULL;
        for(unsigned i=0;i<chunk->metadata.data.length;++i)result=(result^chunk->metadata.data[i])*1099511628211ULL;
    }
    return result;
}
int main(){
    IntCache::CreateNewThreadStorage();
    std::array<int,12> changed{};
    for(auto seed:{0LL,1LL,-1LL,8675309LL})for(int kind=0;kind<12;++kind)for(int fixture=0;fixture<8;++fixture){
        console::GenerationRegion region;
        int anchor=fixture==7?-1:7;
        for(int cx=-2;cx<=1;++cx)for(int cz=-2;cz<=1;++cz){
            auto chunk=std::make_unique<console::ChunkStorage>();
            for(int x=0;x<16;++x)for(int z=0;z<16;++z){
                int column=(x*16+z)*256;
                for(int y=0;y<63;++y)chunk->blocks[column+y]=3;
                chunk->blocks[column+63]=fixture==1?12:fixture==2?9:fixture==3?110:fixture==4?60:2;
                if(fixture==5)chunk->blocks[column+70]=1;
                if(fixture==6 && ((cx*16+x)&3)==0)chunk->blocks[column+63]=9;
                if(fixture==2)chunk->metadata.set(x,63,z,((cx*16+x)&1)?1:0);
                // Controlled fixtures: uniform daylight or a uniformly dark room.
                // These are explicit light inputs, not a propagation approximation.
                for(int y=64;y<256;++y)chunk->skyLight.set(x,y,z,fixture==5?0:15);
            }
            chunk->recalculateHeightmap();region.insert(cx,cz,std::move(chunk));
        }
        if(kind==10)for(int y=64;y<128;++y)region.setTileAndData(anchor+1,y,anchor,17,0);
        region.acceptPreparedLight();
        Level level(seed);level.setBlockAccess(region);Random random(seed);
        std::unique_ptr<Feature> feature;
        switch(kind){
        case 0:feature=std::make_unique<FlowerFeature>(37);break;
        case 1:feature=std::make_unique<FlowerFeature>(38);break;
        case 2:feature=std::make_unique<FlowerFeature>(40);break;
        case 3:feature=std::make_unique<TallGrassFeature>(31,1);break;
        case 4:feature=std::make_unique<TallGrassFeature>(31,2);break;
        case 5:feature=std::make_unique<DeadBushFeature>(32);break;
        case 6:feature=std::make_unique<ReedsFeature>();break;
        case 7:feature=std::make_unique<CactusFeature>();break;
        case 8:feature=std::make_unique<WaterlilyFeature>();break;
        case 9:feature=std::make_unique<PumpkinFeature>();break;
        case 10:feature=std::make_unique<VinesFeature>();break;
        default:feature=std::make_unique<FlowerFeature>(39);break;
        }
        auto before=hash(region);
        bool placed=feature->place(&level,&random,anchor,64,anchor);
        auto after=hash(region);changed[kind]+=before!=after;
        std::cout<<seed<<' '<<kind<<' '<<fixture<<' '<<placed<<' '<<after<<' '<<random.nextLong()<<'\n';
    }
    for(auto count:changed)if(count==0)throw std::runtime_error("Plant fixture never exercised successful placement");
    IntCache::ReleaseThreadStorage();
}
