#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/JavaMath.h"

#include <string>

// Reference: op.ClampFunction (26.3), "minecraft:clamp".

namespace minecraft {
namespace levelgen {
namespace density {

class ClampFunction final : public DensityFunction {
public:
    ClampFunction(DensityFunctionPtr input, float min, float max) : m_input(std::move(input)), m_min(min), m_max(max) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return Interval::clamp(m_input->range(), m_min, m_max); }
    int domainAxes() const override { return m_input->domainAxes(); }
    std::string typeId() const override { return "minecraft:clamp"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const DensityFunctionPtr& input() const { return m_input; }
    float min() const { return m_min; }
    float max() const { return m_max; }

    class Sampler final : public DensitySampler {
    public:
        Sampler(DensitySamplerPtr input, float min, float max) : m_input(std::move(input)), m_min(min), m_max(max) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, jmath::clamp(outputBuffer.get(i), m_min, m_max));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return jmath::clamp(m_input->sampleValue(context, blockX, blockY, blockZ), m_min, m_max);
        }
        const DensitySamplerPtr& input() const { return m_input; }
        float min() const { return m_min; }
        float max() const { return m_max; }
    private:
        DensitySamplerPtr m_input;
        float m_min;
        float m_max;
    };

private:
    DensityFunctionPtr m_input;
    float m_min;
    float m_max;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
