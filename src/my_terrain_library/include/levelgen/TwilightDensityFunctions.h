#pragma once

#include "levelgen/density/DensityFunction.h"
#include "world/biome/TwilightBiomeSource.h"

#include <cstdint>
#include <memory>
#include <string>

// The Twilight Forest's two custom density function types
// (init/TFDensityFunctions.java), driven by the biome layout's TerrainColumn
// grid:
//
//   twilightforest:biome_driven_terrain  TerrainDensityRouter.java
//   twilightforest:biome_driven_noise    NoiseDensityRouter.java
//
// Both sample BiomeDensitySource.sampleTerrain(blockX, blockZ) — the blended
// depth and scale of the surrounding columns — which ignores Y. The mod wraps
// each in a chunk-cached subclass (one 16x16 cache per NoiseChunk, keyed by
// x & 15 / z & 15). Here the memo is a per-thread 16x16 table keyed by the
// layout's uid and the exact column, which gives the same hits inside a
// chunk, is safe for samplers other threads evaluate at single points, and
// lets the two functions share one sample per column.
//
// The mod computes in double on the pre-26.3 engine; on the 26.3 float engine
// the result is narrowed to float at the sampler boundary.

namespace minecraft {
namespace levelgen {

// The blended TerrainColumn depth/scale at a column (memoized per thread).
world::biome::twilight::DensityData sampleTwilightColumn(const world::biome::TwilightBiomeLayout& layout,
                                                         int32_t blockX, int32_t blockZ);

/**
 * TerrainDensityRouter — twilightforest:biome_driven_terrain.
 * value = base_offset + depth * base_factor + depth, with depth the blended
 * TerrainColumn depth. depth_scalar is read by the codec but unused (as in
 * the mod); the bounds are only reported as the range.
 */
class TwilightBiomeDrivenTerrain final : public density::DensityFunction {
public:
    TwilightBiomeDrivenTerrain(std::shared_ptr<const world::biome::TwilightBiomeLayout> layout,
                               double lowerDensityBound, double upperDensityBound, double depthScalar,
                               density::DensityFunctionPtr baseFactor, density::DensityFunctionPtr baseOffset);

    density::DensitySamplerPtr compileSampler(density::CompileContext& context) const override;
    density::DensityFunctionPtr rewriteChildren(const density::DfRewriteRule& rule) const override;
    density::Interval range() const override;
    int domainAxes() const override;
    std::string typeId() const override { return "twilightforest:biome_driven_terrain"; }
    bool equals(const density::DensityFunction& other) const override;
    size_t hash() const override;

private:
    std::shared_ptr<const world::biome::TwilightBiomeLayout> m_layout;
    double m_lowerDensityBound;
    double m_upperDensityBound;
    double m_depthScalar;
    density::DensityFunctionPtr m_baseFactor;
    density::DensityFunctionPtr m_baseOffset;
};

/**
 * NoiseDensityRouter — twilightforest:biome_driven_noise.
 * value = the blended TerrainColumn scale.
 */
class TwilightBiomeDrivenNoise final : public density::DensityFunction {
public:
    TwilightBiomeDrivenNoise(std::shared_ptr<const world::biome::TwilightBiomeLayout> layout,
                             double lowerDensityBound, double upperDensityBound, double depthScalar);

    density::DensitySamplerPtr compileSampler(density::CompileContext& context) const override;
    density::DensityFunctionPtr rewriteChildren(const density::DfRewriteRule&) const override { return self(); }
    density::Interval range() const override;
    int domainAxes() const override { return density::AXIS_X | density::AXIS_Z; }
    std::string typeId() const override { return "twilightforest:biome_driven_noise"; }
    bool equals(const density::DensityFunction& other) const override;
    size_t hash() const override;

private:
    std::shared_ptr<const world::biome::TwilightBiomeLayout> m_layout;
    double m_lowerDensityBound;
    double m_upperDensityBound;
    double m_depthScalar;
};

} // namespace levelgen
} // namespace minecraft
