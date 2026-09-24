#pragma once
#include "BiomeGenerator.h"
#include <array>
#include <memory>
namespace console {
enum class GenerationStage { Density, Surface, Carved };
struct GeneratedChunk {
    // Original storage order: x, z, y; y changes fastest.
    std::array<std::uint8_t,16*16*128> blocks{};
    std::array<std::uint8_t,16*16> biomes{};
};
GeneratedChunk generateFlatChunk();
class ChunkGenerator {
public:
    explicit ChunkGenerator(std::int64_t seed,int worldChunks=54,BiomeScale scale=BiomeScale::Normal);
    ~ChunkGenerator();
    GeneratedChunk generate(int x,int z,GenerationStage stage=GenerationStage::Carved);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
