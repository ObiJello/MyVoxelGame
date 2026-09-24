#include "ChunkGenerator.h"
#include "IntCache.h"
#include <iostream>
int main(){
    IntCache::CreateNewThreadStorage();
    for(auto seed:{0LL,1LL,-1LL,8675309LL}){
        console::ChunkGenerator generator(seed);
        for(auto origin:{std::array<int,2>{0,0},{-1,2},{26,26},{-27,-27},{17,-9}})
        for(auto stage:{console::GenerationStage::Density,console::GenerationStage::Surface,console::GenerationStage::Carved}){
            auto chunk=generator.generate(origin[0],origin[1],stage);
            std::uint64_t hash=14695981039346656037ULL;
            for(auto b:chunk.blocks)hash=(hash^b)*1099511628211ULL;
            for(auto b:chunk.biomes)hash=(hash^b)*1099511628211ULL;
            std::cout<<seed<<' '<<origin[0]<<' '<<origin[1]<<' '<<int(stage)<<' '<<hash<<'\n';
        }
    }
    IntCache::ReleaseThreadStorage();
}
