#pragma once

#include "core/BlockPos.h"
#include "levelgen/WorldGenerationContext.h"
#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DensityFunctionCompiler.h"
#include "levelgen/density/synth/Noise.h"
#include "levelgen/material/MaterialRule.h"
#include "levelgen/material/MaterialRuleContext.h"
#include "random/AnyPositionalRandomFactory.h"

#include <array>
#include <cstdint>

// Reference: levelgen.material.MaterialSystem (26.3) - replaces the default
// block of a noise-filled chunk with the dimension's surface materials: runs
// the compiled material rule over every solid block of every column (top
// down, tracking stone depth above/below and the water line), adds the eroded
// badlands pillars and frozen ocean icebergs, and owns the badlands clay
// bands. One instance per RandomState, shared by every worker; all its
// methods are const and thread-safe (per-pass state lives in the
// MaterialRuleContext).

namespace minecraft {
namespace world {
class IChunk;
namespace biome {
class Biome;
}
} // namespace world

namespace levelgen {
namespace density {
class NoiseChunk;
class RandomState;
} // namespace density

namespace material {

class MaterialSystem {
public:
    static constexpr int32_t CLAY_BAND_COUNT = 192;

    // Java: MaterialSystem(RandomState, BlockState, int, DensityFunction,
    // PositionalRandomFactory); RandomState passes router.chunkSurfaceLevel()
    // and its own positional random.
    MaterialSystem(density::RandomState& randomState, BlockState* defaultBlock, int seaLevel,
                   density::DensityFunctionPtr preliminarySurfaceFunction,
                   random::AnyPositionalRandomFactory noiseRandom);

    MaterialSystem(const MaterialSystem&) = delete;
    MaterialSystem& operator=(const MaterialSystem&) = delete;

    // MaterialSystem.buildSurface. biomeGetter is BiomeManager::getBiome (the
    // fuzzed block biome); possibleBiomes is the ids of the biomes in the
    // palettes of the 3x3 chunks around this one (ChunkStatusTasks
    // .collectPossibleBiomes), or null to skip the BiomeCondition shortcuts.
    // The rule is the registry value of the noise settings' material_rule.
    void buildSurface(density::RandomState& randomState, const BiomeGetter& biomeGetter,
                      const levelgen::WorldGenerationContext& generationContext, world::IChunk* protoChunk,
                      density::NoiseChunk& noiseChunk, const MaterialRule& ruleSource,
                      const PossibleBiomes* possibleBiomes) const;

    // MaterialSystem.topMaterial (deprecated in Java; the carvers' grass
    // re-cover). Evaluates the rule for one block as the top of the surface
    // (stone depth 1 above and below); underFluid puts the water line one block
    // above it. Returns null when the rule places nothing (Optional.empty).
    BlockState* topMaterial(const MaterialRule& ruleSource, density::RandomState& randomState,
                            const levelgen::WorldGenerationContext& worldGenerationContext,
                            const BiomeGetter& biomeGetter, world::IChunk* chunk,
                            const density::DensitySamplerSet& densitySamplers, const core::BlockPos& pos,
                            bool underFluid) const;

    int32_t getSurfaceDepth(int32_t blockX, int32_t blockZ) const;
    double getSurfaceSecondary(int32_t blockX, int32_t blockZ) const;
    int32_t getSeaLevel() const { return m_seaLevel; }
    BlockState* getBand(int32_t worldX, int32_t y, int32_t worldZ) const;
    const density::DensityFunctionPtr& preliminarySurfaceFunction() const { return m_preliminarySurfaceFunction; }

    BlockState* defaultBlock() const { return m_defaultBlock; }
    const std::array<BlockState*, CLAY_BAND_COUNT>& clayBands() const { return m_clayBands; }

    // The engine's WorldGenerationContext with this system's sea level (the
    // generator's), as the material rules resolve anchors against it.
    GenerationContext generationContext(const levelgen::WorldGenerationContext& context) const;

private:
    class ChunkColumn;

    static int32_t getSurfaceGradientX(world::IChunk* protoChunk, int32_t x, int32_t z);
    static int32_t getSurfaceGradientZ(world::IChunk* protoChunk, int32_t x, int32_t z);
    static std::array<BlockState*, CLAY_BAND_COUNT> generateBands(random::AnyRandomSource& random);
    static void makeBands(random::AnyRandomSource& random, std::array<BlockState*, CLAY_BAND_COUNT>& clayBands,
                          int32_t baseWidth, BlockState* state);

    void erodedBadlandsExtension(ChunkColumn& column, int32_t blockX, int32_t blockZ, int32_t height,
                                 world::IChunk* protoChunk) const;
    void frozenOceanExtension(int32_t minSurfaceLevel, const world::biome::Biome* surfaceBiome, ChunkColumn& column,
                              core::BlockPos::MutableBlockPos& blockPos, int32_t blockX, int32_t blockZ,
                              int32_t height) const;

    BlockState* m_defaultBlock;
    int32_t m_seaLevel;
    density::DensityFunctionPtr m_preliminarySurfaceFunction;
    std::array<BlockState*, CLAY_BAND_COUNT> m_clayBands{};
    density::synth::NoisePtr m_clayBandsOffsetNoise;
    density::synth::NoisePtr m_badlandsPillarNoise;
    density::synth::NoisePtr m_badlandsPillarRoofNoise;
    density::synth::NoisePtr m_badlandsSurfaceNoise;
    density::synth::NoisePtr m_icebergPillarNoise;
    density::synth::NoisePtr m_icebergPillarRoofNoise;
    density::synth::NoisePtr m_icebergSurfaceNoise;
    random::AnyPositionalRandomFactory m_noiseRandom;
    density::synth::NoisePtr m_surfaceNoise;
    density::synth::NoisePtr m_surfaceSecondaryNoise;
};

} // namespace material
} // namespace levelgen
} // namespace minecraft
