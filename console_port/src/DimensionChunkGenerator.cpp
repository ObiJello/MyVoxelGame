#include "DimensionChunkGenerator.h"
#include "HellRandomLevelSource.h"
#include "TheEndLevelRandomLevelSource.h"
#include "LargeHellCaveFeature.h"
namespace console {
struct DimensionChunkGenerator::Impl {
    Level level;
    TerrainDimension dimension;
    std::unique_ptr<HellRandomLevelSource> hell;
    std::unique_ptr<TheEndLevelRandomLevelSource> end;
    LargeHellCaveFeature caves;
    std::mutex mutex;
    Impl(TerrainDimension kind,std::int64_t seed,int chunks,int scale):level(seed,chunks),dimension(kind){
        level.getLevelData()->hellScale=scale;
        if(kind==TerrainDimension::Nether){level.dimension->hasCeiling=true;hell=std::make_unique<HellRandomLevelSource>(&level,seed);}
        else end=std::make_unique<TheEndLevelRandomLevelSource>(&level,seed);
    }
};
DimensionChunkGenerator::DimensionChunkGenerator(TerrainDimension kind,std::int64_t seed,int worldChunks,int hellScale){
    if(kind!=TerrainDimension::Nether && kind!=TerrainDimension::End)throw std::invalid_argument("Unsupported terrain dimension");
    if(worldChunks<4 || worldChunks>2048 || worldChunks%2 || hellScale<1 || hellScale>16)throw std::invalid_argument("Invalid finite-world or Nether scale setting");
    Mth::init();impl_=std::make_unique<Impl>(kind,seed,worldChunks,hellScale);
}
DimensionChunkGenerator::~DimensionChunkGenerator()=default;
GeneratedChunk DimensionChunkGenerator::generate(int x,int z,GenerationStage stage){
    if(x<-1000000 || z<-1000000 || x>1000000 || z>1000000)throw std::invalid_argument("Chunk coordinates outside supported range");
    if(stage!=GenerationStage::Density && stage!=GenerationStage::Surface && stage!=GenerationStage::Carved)throw std::invalid_argument("Unknown generation stage");
    std::lock_guard lock(impl_->mutex);GeneratedChunk result;byteArray blocks(result.blocks.data(),result.blocks.size());
    if(impl_->hell){
        auto& source=*impl_->hell;source.seedChunk(x,z);source.prepareHeights(x,z,blocks);
        if(stage!=GenerationStage::Density)source.buildSurfaces(x,z,blocks);
        if(stage==GenerationStage::Carved)impl_->caves.apply(nullptr,&impl_->level,x,z,blocks);
        result.biomes.fill(Biome::hell->id);
    }else{
        auto& source=*impl_->end;source.seedChunk(x,z);source.prepareHeights(x,z,blocks,BiomeArray());
        if(stage!=GenerationStage::Density)source.buildSurfaces(x,z,blocks,BiomeArray());
        result.biomes.fill(Biome::sky->id);
    }
    return result;
}
}
