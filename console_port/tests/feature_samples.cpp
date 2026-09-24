#include "stdafx.h"
#include "BasicTree.h"
#include "GenerationRegion.h"
#include "WorldGenLevel.h"
#include "TreeFeature.h"
#include "BirchFeature.h"
#include "PineFeature.h"
#include "SpruceFeature.h"
#include "OreFeature.h"
#include "SwampTreeFeature.h"
#include "MegaTreeFeature.h"
#include "GroundBushFeature.h"
#include "HugeMushroomFeature.h"
#include "ClayFeature.h"
#include "SandFeature.h"
#include "DesertWellFeature.h"

#include "Mth.h"
#include "IntCache.h"
#include <iostream>
int main(){
    Mth::init();IntCache::CreateNewThreadStorage();
    std::array<int,16> modified{};
    auto hashRegion=[](const console::GenerationRegion& region){
        std::uint64_t hash=14695981039346656037ULL;
        for(const auto& [position,chunk]:region.chunks){
            for(auto value:chunk->blocks)hash=(hash^value)*1099511628211ULL;
            for(auto value:chunk->heightmap)hash=(hash^value)*1099511628211ULL;
            for(unsigned i=0;i<chunk->metadata.data.length;++i)hash=(hash^chunk->metadata.data[i])*1099511628211ULL;
        }
        return hash;
    };
    for(auto seed:{0LL,1LL,-1LL,8675309LL})for(int kind=0;kind<16;++kind)for(int fixture=0;fixture<7;++fixture){
        console::GenerationRegion region;
        int floor=fixture==5?245:63,anchor=fixture==6?-1:7;
        for(int cx=-1;cx<=2;++cx)for(int cz=-1;cz<=2;++cz){
            auto chunk=std::make_unique<console::ChunkStorage>();
            for(int column=0;column<256;++column)for(int y=0;y<=floor;++y)chunk->blocks[column*256+y]=y==floor?(fixture==2?12:fixture==4?110:fixture==3?3:2):y>=floor-3?3:1;
            if(fixture==3)for(int column=0;column<256;++column)chunk->blocks[column*256+floor+1]=9;
            chunk->recalculateHeightmap();region.insert(cx,cz,std::move(chunk));
        }
        if(fixture==1)region.setTileAndData(anchor,floor+2,anchor,1,0);
        Level level(seed);level.setBlockAccess(region);Random random(seed);
        std::unique_ptr<Feature> feature;
        switch(kind){
        case 0:feature=std::make_unique<TreeFeature>(false);break;
        case 1:feature=std::make_unique<TreeFeature>(false,7,3,3,true);break;
        case 2:feature=std::make_unique<BirchFeature>(false);break;
        case 3:feature=std::make_unique<PineFeature>();break;
        case 4:feature=std::make_unique<SpruceFeature>(false);break;
        case 5:feature=std::make_unique<OreFeature>(16,16);break;
        case 6:feature=std::make_unique<OreFeature>(56,7);break;
        case 7:feature=std::make_unique<SwampTreeFeature>();break;
        case 8:feature=std::make_unique<MegaTreeFeature>(false,12,3,3);break;
        case 9:feature=std::make_unique<GroundBushFeature>(3,0);break;
        case 10:feature=std::make_unique<HugeMushroomFeature>();break;
        case 11:feature=std::make_unique<ClayFeature>(4);break;
        case 12:feature=std::make_unique<SandFeature>(7,12);break;
        case 13:feature=std::make_unique<SandFeature>(6,13);break;
        case 15:feature=std::make_unique<BasicTree>(false);feature->init(1,1,1);break;
        default:feature=std::make_unique<DesertWellFeature>();break;
        }
        auto before=hashRegion(region);
        bool placed=feature->place(&level,&random,anchor,(kind==5 || kind==6)?20:floor+1,anchor);
        if(level.getInstaTick())throw std::runtime_error("Feature leaked instaTick state");
        auto hash=hashRegion(region);
        if(before!=hash)++modified[kind];
        std::cout<<seed<<' '<<kind<<' '<<fixture<<' '<<placed<<' '<<hash<<' '<<random.nextLong()<<'\n';
    }
    for(auto count:modified)if(count==0)throw std::runtime_error("Feature fixtures did not exercise any successful placement");
    IntCache::ReleaseThreadStorage();
}
