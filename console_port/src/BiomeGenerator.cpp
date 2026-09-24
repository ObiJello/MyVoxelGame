#include "BiomeGenerator.h"
#include "Layer.h"
#include "LevelType.h"
#include "IntCache.h"
#include <stdexcept>
namespace console {
BiomeGenerator::BiomeGenerator(std::int64_t seed,BiomeScale scale) {
    static std::once_flag initialized;
    std::call_once(initialized,[]{LevelType::staticCtor();});
    auto type=scale==BiomeScale::Large?LevelType::lvl_largeBiomes:scale==BiomeScale::Legacy11?LevelType::lvl_normal_1_1:LevelType::lvl_normal;
    auto layers=Layer::getDefaultLayers(seed,type);
    raw_=layers[0];zoomed_=layers[1];delete[] layers.data;
}
std::vector<std::uint8_t> BiomeGenerator::area(int x,int z,int width,int depth,bool raw) {
    if(width<1 || depth<1 || width>2048 || depth>2048 || x<-30000000 || z<-30000000 ||
        x>30000000-width || z>30000000-depth)throw std::invalid_argument("Biome area outside supported bounds");
    std::lock_guard lock(mutex_);
    IntCache::releaseAll();
    auto area=(raw?raw_:zoomed_)->getArea(x,z,width,depth);
    std::vector<std::uint8_t> result(width*depth);
    for(std::size_t i=0;i<result.size();++i){
        if(area[i]<0 || area[i]>22)throw std::runtime_error("Original layer returned an invalid biome");
        result[i]=static_cast<std::uint8_t>(area[i]);
    }
    return result;
}
}
