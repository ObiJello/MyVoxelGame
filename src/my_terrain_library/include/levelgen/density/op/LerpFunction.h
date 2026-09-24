#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/SamplerContext.h"

#include <string>

// Reference: op.LerpFunction (26.3) - "minecraft:lerp". Its samplers are
// public (synth.BlendedNoise builds a LerpFunction::Sampler directly).

namespace minecraft {
namespace levelgen {
namespace density {

class LerpFunction final : public DensityFunction {
public:
    LerpFunction(DensityFunctionPtr alpha, DensityFunctionPtr first, DensityFunctionPtr second)
        : m_alpha(std::move(alpha)), m_first(std::move(first)), m_second(std::move(second)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override;
    std::string typeId() const override { return "minecraft:lerp"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const DensityFunctionPtr& alpha() const { return m_alpha; }
    const DensityFunctionPtr& first() const { return m_first; }
    const DensityFunctionPtr& second() const { return m_second; }

    class ConstFirstSampler final : public DensitySampler {
    public:
        ConstFirstSampler(DensitySamplerPtr alpha, float first, DensitySamplerPtr second)
            : m_alpha(std::move(alpha)), m_first(first), m_second(std::move(second)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                          const DensityVolume& volume) const override;
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override;
        const DensitySamplerPtr& alpha() const { return m_alpha; }
        float first() const { return m_first; }
        const DensitySamplerPtr& second() const { return m_second; }
    private:
        DensitySamplerPtr m_alpha;
        float m_first;
        DensitySamplerPtr m_second;
    };

    class ConstSecondSampler final : public DensitySampler {
    public:
        ConstSecondSampler(DensitySamplerPtr alpha, DensitySamplerPtr first, float second)
            : m_alpha(std::move(alpha)), m_first(std::move(first)), m_second(second) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                          const DensityVolume& volume) const override;
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override;
        const DensitySamplerPtr& alpha() const { return m_alpha; }
        const DensitySamplerPtr& first() const { return m_first; }
        float second() const { return m_second; }
    private:
        DensitySamplerPtr m_alpha;
        DensitySamplerPtr m_first;
        float m_second;
    };

    class Sampler final : public DensitySampler {
    public:
        Sampler(DensitySamplerPtr alpha, DensitySamplerPtr first, DensitySamplerPtr second)
            : m_alpha(std::move(alpha)), m_first(std::move(first)), m_second(std::move(second)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                          const DensityVolume& volume) const override;
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override;
        const DensitySamplerPtr& alpha() const { return m_alpha; }
        const DensitySamplerPtr& first() const { return m_first; }
        const DensitySamplerPtr& second() const { return m_second; }
    private:
        DensitySamplerPtr m_alpha;
        DensitySamplerPtr m_first;
        DensitySamplerPtr m_second;
    };

private:
    DensityFunctionPtr m_alpha;
    DensityFunctionPtr m_first;
    DensityFunctionPtr m_second;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
