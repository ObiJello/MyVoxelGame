#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/synth/Noise.h"

#include <string>

// Reference: generator.NoiseFunction (26.3), "minecraft:noise" - a NormalNoise
// sampled at (x * xzScale + shiftX, y * yScale + shiftY, z * xzScale + shiftZ).
// The unshifted sampler fills a volume through Noise.addToVolume (the noise's
// own volume walk); the shifted ones sample point by point.

namespace minecraft {
namespace levelgen {
namespace density {

class NoiseFunction final : public DensityFunction {
public:
    NoiseFunction(NoiseHolder noise, double xzScale, double yScale, DensityFunctionPtr shiftX,
                  DensityFunctionPtr shiftY, DensityFunctionPtr shiftZ)
        : m_noise(std::move(noise)), m_xzScale(xzScale), m_yScale(yScale), m_shiftX(std::move(shiftX)),
          m_shiftY(std::move(shiftY)), m_shiftZ(std::move(shiftZ)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override;
    std::string typeId() const override { return "minecraft:noise"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const NoiseHolder& noise() const { return m_noise; }
    double xzScale() const { return m_xzScale; }
    double yScale() const { return m_yScale; }
    const DensityFunctionPtr& shiftX() const { return m_shiftX; }
    const DensityFunctionPtr& shiftY() const { return m_shiftY; }
    const DensityFunctionPtr& shiftZ() const { return m_shiftZ; }

    class Sampler final : public DensitySampler {
    public:
        Sampler(synth::NoisePtr noise, double xzScale, double yScale)
            : m_noise(std::move(noise)), m_xzScale(xzScale), m_yScale(yScale) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override;
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override;
        const synth::NoisePtr& noise() const { return m_noise; }
        double xzScale() const { return m_xzScale; }
        double yScale() const { return m_yScale; }
    private:
        synth::NoisePtr m_noise;
        double m_xzScale;
        double m_yScale;
    };

    class ShiftedXzSampler final : public DensitySampler {
    public:
        ShiftedXzSampler(DensitySamplerPtr shiftX, DensitySamplerPtr shiftZ, synth::NoisePtr noise, double xzScale,
                         double yScale)
            : m_shiftX(std::move(shiftX)), m_shiftZ(std::move(shiftZ)), m_noise(std::move(noise)),
              m_xzScale(xzScale), m_yScale(yScale) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override;
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override;
    private:
        DensitySamplerPtr m_shiftX;
        DensitySamplerPtr m_shiftZ;
        synth::NoisePtr m_noise;
        double m_xzScale;
        double m_yScale;
    };

    class ShiftedXyzSampler final : public DensitySampler {
    public:
        ShiftedXyzSampler(DensitySamplerPtr shiftX, DensitySamplerPtr shiftY, DensitySamplerPtr shiftZ,
                          synth::NoisePtr noise, double xzScale, double yScale)
            : m_shiftX(std::move(shiftX)), m_shiftY(std::move(shiftY)), m_shiftZ(std::move(shiftZ)),
              m_noise(std::move(noise)), m_xzScale(xzScale), m_yScale(yScale) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override;
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override;
    private:
        DensitySamplerPtr m_shiftX;
        DensitySamplerPtr m_shiftY;
        DensitySamplerPtr m_shiftZ;
        synth::NoisePtr m_noise;
        double m_xzScale;
        double m_yScale;
    };

private:
    NoiseHolder m_noise;
    double m_xzScale;
    double m_yScale;
    DensityFunctionPtr m_shiftX;
    DensityFunctionPtr m_shiftY;
    DensityFunctionPtr m_shiftZ;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
