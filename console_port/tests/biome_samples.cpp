#include "stdafx.h"
#include "Layer.h"
#include "LevelType.h"
#include "IntCache.h"
#include <iostream>
#include <array>
int main(){
    LevelType::staticCtor();IntCache::CreateNewThreadStorage();
    for(auto type:{LevelType::lvl_normal,LevelType::lvl_largeBiomes,LevelType::lvl_normal_1_1})
    for(std::int64_t seed:{0LL,1LL,-1LL,8675309LL,9223372036854775807LL}) {
        auto layers=Layer::getDefaultLayers(seed,type);
        for(auto origin:{std::array<int,2>{0,0},{-433,-433},{-17,31},{432,432},{-16777217,16777219}})
        for(int raw:{0,1}) {
            IntCache::releaseAll();int w=37,h=43;
            auto area=layers[raw]->getArea(origin[0],origin[1],w,h);
            std::uint64_t hash=14695981039346656037ULL;
            for(int i=0;i<w*h;++i)hash=(hash^static_cast<std::uint32_t>(area[i]))*1099511628211ULL;
            std::cout<<seed<<' '<<type->getVersion()<<' '<<origin[0]<<' '<<origin[1]<<' '<<raw<<' '<<hash<<'\n';
        }
        delete[] layers.data;
    }
    IntCache::ReleaseThreadStorage();
    for(auto type:LevelType::levelTypes)delete type;
}
