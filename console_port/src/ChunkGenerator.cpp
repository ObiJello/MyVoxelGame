#include "ChunkGenerator.h"
#include "RandomLevelSource.h"
#include "LargeCaveFeature.h"
#include "CanyonFeature.h"
#include "Mth.h"
#include "IntCache.h"
#include <mutex>
namespace console {
struct ChunkGenerator::Impl {
    Level level;
    RandomLevelSource source;
    LargeCaveFeature caves;
    CanyonFeature canyon;
    std::mutex mutex;
    Impl(std::int64_t seed,int chunks,BiomeScale scale):level(seed,chunks,scale),source(&level,seed){}
};
ChunkGenerator::ChunkGenerator(std::int64_t seed,int chunks,BiomeScale scale){
    if(chunks<4 || chunks>2048 || chunks%2)throw std::invalid_argument("Finite world size must be an even chunk count from 4 to 2048");
    static std::once_flag mathInitialized;
    std::call_once(mathInitialized,[]{Mth::init();});
    impl_=std::make_unique<Impl>(seed,chunks,scale);
}
ChunkGenerator::~ChunkGenerator()=default;
GeneratedChunk ChunkGenerator::generate(int x,int z,GenerationStage stage){
    if(x<-1000000 || z<-1000000 || x>1000000 || z>1000000)throw std::invalid_argument("Chunk coordinates outside supported range");
    std::lock_guard lock(impl_->mutex);
    auto& level=impl_->level;auto& source=impl_->source;
    GeneratedChunk result;byteArray blocks(result.blocks.data(),result.blocks.size());
    source.seedChunk(x,z);source.prepareHeights(x,z,blocks);
    BiomeArray biomes;level.getBiomeSource()->getBiomeBlock(biomes,x*16,z*16,16,16,true);
    std::unique_ptr<Biome*[]> biomeOwner(biomes.data);
    for(std::size_t i=0;i<result.biomes.size();++i)result.biomes[i]=biomes[i]->id;
    if(stage!=GenerationStage::Density)source.buildSurfaces(x,z,blocks,biomes);
    if(stage==GenerationStage::Carved){impl_->caves.apply(nullptr,&level,x,z,blocks);impl_->canyon.apply(nullptr,&level,x,z,blocks);}
    return result;
}
}
