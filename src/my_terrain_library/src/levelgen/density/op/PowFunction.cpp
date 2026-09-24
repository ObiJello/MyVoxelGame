#include "levelgen/density/op/PowFunction.h"

#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/op/UnaryFunction.h"

#include <cmath>

// Reference: op.PowFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

class ConstBaseSampler final : public DensitySampler {
public:
    ConstBaseSampler(double base, DensitySamplerPtr exponent) : m_base(base), m_exponent(std::move(exponent)) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_exponent->sampleVolume(context, outputBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            outputBuffer.set(i, static_cast<float>(std::pow(m_base, static_cast<double>(outputBuffer.get(i)))));
        }
    }
    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        return static_cast<float>(
            std::pow(m_base, static_cast<double>(m_exponent->sampleValue(context, blockX, blockY, blockZ))));
    }
private:
    double m_base;
    DensitySamplerPtr m_exponent;
};

class Sampler final : public DensitySampler {
public:
    Sampler(DensitySamplerPtr base, DensitySamplerPtr exponent) : m_base(std::move(base)), m_exponent(std::move(exponent)) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_base->sampleVolume(context, outputBuffer, volume);
        ScopedBuffer exponentBuffer = context.acquireBuffer(volume);
        m_exponent->sampleVolume(context, *exponentBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            const float base = outputBuffer.get(i);
            const float exponent = exponentBuffer->get(i);
            outputBuffer.set(i, static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponent))));
        }
    }
    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        return static_cast<float>(std::pow(static_cast<double>(m_base->sampleValue(context, blockX, blockY, blockZ)),
                                           static_cast<double>(m_exponent->sampleValue(context, blockX, blockY, blockZ))));
    }
private:
    DensitySamplerPtr m_base;
    DensitySamplerPtr m_exponent;
};

class ConstExponentSampler final : public DensitySampler {
public:
    ConstExponentSampler(DensitySamplerPtr base, double exponent) : m_base(std::move(base)), m_exponent(exponent) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_base->sampleVolume(context, outputBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            outputBuffer.set(i, static_cast<float>(std::pow(static_cast<double>(outputBuffer.get(i)), m_exponent)));
        }
    }
    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        return static_cast<float>(
            std::pow(static_cast<double>(m_base->sampleValue(context, blockX, blockY, blockZ)), m_exponent));
    }
private:
    DensitySamplerPtr m_base;
    double m_exponent;
};

} // namespace

DensitySamplerPtr PowFunction::compileSampler(CompileContext& context) const {
    DensitySamplerPtr base = m_base->compileSampler(context);
    DensitySamplerPtr exponent = m_exponent->compileSampler(context);
    if (const ConstantFunction* c = asConstant(m_base)) {
        const float baseValue = c->value();
        return std::make_shared<ConstBaseSampler>(static_cast<double>(baseValue), exponent);
    }
    if (const ConstantFunction* c = asConstant(m_exponent)) {
        const float exponentValue = c->value();
        return compileConstExponent(base, exponentValue);
    }
    return std::make_shared<Sampler>(base, exponent);
}

DensitySamplerPtr PowFunction::compileConstExponent(const DensitySamplerPtr& base, float exponent) {
    const float absExponent = std::fabs(exponent);
    DensitySamplerPtr specialSampler;
    if (absExponent == 0.5f) {
        specialSampler = std::make_shared<UnaryFunction::SqrtSampler>(base);
    } else if (absExponent == 1.0f) {
        specialSampler = base;
    } else if (absExponent == 2.0f) {
        specialSampler = std::make_shared<UnaryFunction::SquareSampler>(base);
    } else {
        if (absExponent != 3.0f) {
            return std::make_shared<ConstExponentSampler>(base, static_cast<double>(exponent));
        }
        specialSampler = std::make_shared<UnaryFunction::CubeSampler>(base);
    }
    return exponent >= 0.0f ? specialSampler : std::make_shared<UnaryFunction::ReciprocalSampler>(specialSampler);
}

DensityFunctionPtr PowFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr base = rule.rewrite(m_base);
    DensityFunctionPtr exponent = rule.rewrite(m_exponent);
    return base == m_base && exponent == m_exponent ? self() : std::make_shared<PowFunction>(base, exponent);
}

bool PowFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const PowFunction*>(&other);
    return o != nullptr && functionsEqual(m_base, o->m_base) && functionsEqual(m_exponent, o->m_exponent);
}

size_t PowFunction::hash() const {
    size_t h = hashCombine(0x34, m_base->hash());
    return hashCombine(h, m_exponent->hash());
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
