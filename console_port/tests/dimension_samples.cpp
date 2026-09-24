#include "DimensionChunkGenerator.h"
#include <iostream>
int main(){
    for(auto dimension:{console::TerrainDimension::Nether,console::TerrainDimension::End})for(auto seed:{0ll,1ll,-1ll,9223372036854775807ll})for(auto [chunks,scale]:{std::pair{54,3},std::pair{50,3}}){
        console::DimensionChunkGenerator generator(dimension,seed,chunks,scale);
        for(auto [x,z]:{std::pair{0,0},std::pair{-1,2},std::pair{-9,0},std::pair{8,0},std::pair{0,-9},std::pair{0,8},std::pair{-10,10},std::pair{27,-27}})for(auto stage:{console::GenerationStage::Density,console::GenerationStage::Surface,console::GenerationStage::Carved}){
            auto chunk=generator.generate(x,z,stage);std::uint64_t hash=14695981039346656037ull;for(auto b:chunk.blocks)hash=(hash^b)*1099511628211ull;for(auto b:chunk.biomes)hash=(hash^b)*1099511628211ull;
            std::cout<<int(dimension)<<' '<<seed<<' '<<chunks<<' '<<scale<<' '<<x<<' '<<z<<' '<<int(stage)<<' '<<hash<<'\n';
        }
    }
}
