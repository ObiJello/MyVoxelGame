#include "levelgen/TwilightDensityFunctions.h"

#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/SamplerContext.h"

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

namespace minecraft {
namespace levelgen {

using world::biome::TwilightBiomeLayout;
using world::biome::twilight::DensityData;

namespace {

// One entry per column of a 16x16 block area. uid 0 is never issued, so a
// zero-initialised entry is empty.
struct ColumnMemo {
    uint64_t uid = 0;
    int32_t x = 0;
    int32_t z = 0;
    DensityData data{0.0, 0.0};
};

thread_local std::array<ColumnMemo, 256> t_columnMemo{};

class TerrainSampler final : public density::DensitySampler {
public:
    TerrainSampler(std::shared_ptr<const TwilightBiomeLayout> layout, density::DensitySamplerPtr baseFactor,
                   density::DensitySamplerPtr baseOffset)
        : m_layout(std::move(layout)), m_baseFactor(std::move(baseFactor)), m_baseOffset(std::move(baseOffset)) {}

    void sampleVolume(density::SamplerContext& context, density::DensityBuffer& outputBuffer,
                      const density::DensityVolume& volume) const override {
        sampleVolumeNaive(context, outputBuffer, volume, *this);
    }

    float sampleValue(density::SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const DensityData densityData = sampleTwilightColumn(*m_layout, blockX, blockZ);
        const double offset = m_baseOffset->sampleValue(context, blockX, blockY, blockZ);
        const double factor = m_baseFactor->sampleValue(context, blockX, blockY, blockZ);
        const double depth = offset + densityData.depth * factor;
        return static_cast<float>(depth + densityData.depth);
    }

private:
    std::shared_ptr<const TwilightBiomeLayout> m_layout;
    density::DensitySamplerPtr m_baseFactor;
    density::DensitySamplerPtr m_baseOffset;
};

class NoiseSampler final : public density::DensitySampler {
public:
    explicit NoiseSampler(std::shared_ptr<const TwilightBiomeLayout> layout) : m_layout(std::move(layout)) {}

    void sampleVolume(density::SamplerContext&, density::DensityBuffer& outputBuffer,
                      const density::DensityVolume& volume) const override {
        for (int z = 0; z < volume.sizeZ; ++z) {
            const int blockZ = volume.blockZ(z);
            for (int x = 0; x < volume.sizeX; ++x) {
                const float value = scaleAt(volume.blockX(x), blockZ);
                outputBuffer.setRange(volume.indexUnchecked(x, 0, z), volume.sizeY, value);
            }
        }
    }

    float sampleValue(density::SamplerContext&, int blockX, int, int blockZ) const override {
        return scaleAt(blockX, blockZ);
    }

private:
    float scaleAt(int blockX, int blockZ) const {
        return static_cast<float>(sampleTwilightColumn(*m_layout, blockX, blockZ).scale);
    }

    std::shared_ptr<const TwilightBiomeLayout> m_layout;
};

} // namespace

DensityData sampleTwilightColumn(const TwilightBiomeLayout& layout, int32_t blockX, int32_t blockZ) {
    ColumnMemo& memo = t_columnMemo[static_cast<size_t>((blockX & 15) | ((blockZ & 15) << 4))];
    if (memo.uid == layout.uid() && memo.x == blockX && memo.z == blockZ) {
        return memo.data;
    }
    memo.data = layout.sampleTerrain(blockX, blockZ);
    memo.uid = layout.uid();
    memo.x = blockX;
    memo.z = blockZ;
    return memo.data;
}

// ============================================================================
// TerrainDensityRouter
// ============================================================================

TwilightBiomeDrivenTerrain::TwilightBiomeDrivenTerrain(
    std::shared_ptr<const TwilightBiomeLayout> layout, double lowerDensityBound, double upperDensityBound,
    double depthScalar, density::DensityFunctionPtr baseFactor, density::DensityFunctionPtr baseOffset)
    : m_layout(std::move(layout)),
      m_lowerDensityBound(lowerDensityBound),
      m_upperDensityBound(upperDensityBound),
      m_depthScalar(depthScalar),
      m_baseFactor(std::move(baseFactor)),
      m_baseOffset(std::move(baseOffset)) {}

density::DensitySamplerPtr TwilightBiomeDrivenTerrain::compileSampler(density::CompileContext& context) const {
    density::DensitySamplerPtr baseFactor = m_baseFactor->compileSampler(context);
    density::DensitySamplerPtr baseOffset = m_baseOffset->compileSampler(context);
    return std::make_shared<TerrainSampler>(m_layout, std::move(baseFactor), std::move(baseOffset));
}

density::DensityFunctionPtr TwilightBiomeDrivenTerrain::rewriteChildren(const density::DfRewriteRule& rule) const {
    density::DensityFunctionPtr baseFactor = rule.rewrite(m_baseFactor);
    density::DensityFunctionPtr baseOffset = rule.rewrite(m_baseOffset);
    if (baseFactor == m_baseFactor && baseOffset == m_baseOffset) return self();
    return std::make_shared<TwilightBiomeDrivenTerrain>(m_layout, m_lowerDensityBound, m_upperDensityBound,
                                                        m_depthScalar, baseFactor, baseOffset);
}

density::Interval TwilightBiomeDrivenTerrain::range() const {
    return density::Interval::of(static_cast<float>(m_lowerDensityBound), static_cast<float>(m_upperDensityBound));
}

int TwilightBiomeDrivenTerrain::domainAxes() const {
    return density::AXIS_X | density::AXIS_Z | m_baseFactor->domainAxes() | m_baseOffset->domainAxes();
}

bool TwilightBiomeDrivenTerrain::equals(const density::DensityFunction& other) const {
    const auto* o = dynamic_cast<const TwilightBiomeDrivenTerrain*>(&other);
    return o != nullptr && o->m_layout == m_layout && density::doubleEq(o->m_lowerDensityBound, m_lowerDensityBound) &&
           density::doubleEq(o->m_upperDensityBound, m_upperDensityBound) &&
           density::doubleEq(o->m_depthScalar, m_depthScalar) && density::functionsEqual(o->m_baseFactor, m_baseFactor) &&
           density::functionsEqual(o->m_baseOffset, m_baseOffset);
}

size_t TwilightBiomeDrivenTerrain::hash() const {
    size_t h = density::hashCombine(0x7401, std::hash<const void*>()(m_layout.get()));
    h = density::hashCombine(h, density::hashDouble(m_lowerDensityBound));
    h = density::hashCombine(h, density::hashDouble(m_upperDensityBound));
    h = density::hashCombine(h, m_baseFactor->hash());
    return density::hashCombine(h, m_baseOffset->hash());
}

// ============================================================================
// NoiseDensityRouter
// ============================================================================

TwilightBiomeDrivenNoise::TwilightBiomeDrivenNoise(std::shared_ptr<const TwilightBiomeLayout> layout,
                                                   double lowerDensityBound, double upperDensityBound,
                                                   double depthScalar)
    : m_layout(std::move(layout)),
      m_lowerDensityBound(lowerDensityBound),
      m_upperDensityBound(upperDensityBound),
      m_depthScalar(depthScalar) {}

density::DensitySamplerPtr TwilightBiomeDrivenNoise::compileSampler(density::CompileContext&) const {
    return std::make_shared<NoiseSampler>(m_layout);
}

density::Interval TwilightBiomeDrivenNoise::range() const {
    return density::Interval::of(static_cast<float>(m_lowerDensityBound), static_cast<float>(m_upperDensityBound));
}

bool TwilightBiomeDrivenNoise::equals(const density::DensityFunction& other) const {
    const auto* o = dynamic_cast<const TwilightBiomeDrivenNoise*>(&other);
    return o != nullptr && o->m_layout == m_layout && density::doubleEq(o->m_lowerDensityBound, m_lowerDensityBound) &&
           density::doubleEq(o->m_upperDensityBound, m_upperDensityBound) &&
           density::doubleEq(o->m_depthScalar, m_depthScalar);
}

size_t TwilightBiomeDrivenNoise::hash() const {
    size_t h = density::hashCombine(0x7402, std::hash<const void*>()(m_layout.get()));
    h = density::hashCombine(h, density::hashDouble(m_lowerDensityBound));
    return density::hashCombine(h, density::hashDouble(m_upperDensityBound));
}

} // namespace levelgen
} // namespace minecraft
