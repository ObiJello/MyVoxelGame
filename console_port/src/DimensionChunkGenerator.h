#pragma once
#include "ChunkGenerator.h"
namespace console {
enum class TerrainDimension { Nether=-1,End=1 };
// Original non-overworld terrain arrays. No fortresses, decoration, entity
// spawning or dimension transitions are implied by these generation stages.
class DimensionChunkGenerator {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    DimensionChunkGenerator(TerrainDimension,std::int64_t seed,int worldChunks=54,int hellScale=3);
    ~DimensionChunkGenerator();
    GeneratedChunk generate(int x,int z,GenerationStage stage=GenerationStage::Carved);
};
}
