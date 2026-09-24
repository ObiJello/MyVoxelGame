#include "levelgen/density/op/RangeChoiceFunction.h"

#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/SamplerContext.h"

#include <vector>

// Reference: op.RangeChoiceFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// RangeChoiceFunction.ConstSampler: both branches constant.
class RangeChoiceConstSampler final : public DensitySampler {
public:
    RangeChoiceConstSampler(DensitySamplerPtr input, float minInclusive, float maxExclusive, float whenInRange,
                 float whenOutOfRange)
        : m_input(std::move(input)), m_minInclusive(minInclusive), m_maxExclusive(maxExclusive),
          m_whenInRange(whenInRange), m_whenOutOfRange(whenOutOfRange) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_input->sampleVolume(context, outputBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            outputBuffer.set(i, choose(outputBuffer.get(i)));
        }
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        return choose(m_input->sampleValue(context, blockX, blockY, blockZ));
    }

private:
    float choose(float input) const {
        return input >= m_minInclusive && input < m_maxExclusive ? m_whenInRange : m_whenOutOfRange;
    }

    DensitySamplerPtr m_input;
    float m_minInclusive;
    float m_maxExclusive;
    float m_whenInRange;
    float m_whenOutOfRange;
};

// RangeChoiceFunction.Sampler.
class RangeChoiceSampler final : public DensitySampler {
public:
    RangeChoiceSampler(DensitySamplerPtr input, float minInclusive, float maxExclusive, DensitySamplerPtr whenInRange,
            DensitySamplerPtr whenOutOfRange)
        : m_input(std::move(input)), m_minInclusive(minInclusive), m_maxExclusive(maxExclusive),
          m_whenInRange(std::move(whenInRange)), m_whenOutOfRange(std::move(whenOutOfRange)) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_whenInRange->sampleVolume(context, outputBuffer, volume);
        ScopedBuffer inputBuffer = context.acquireBuffer(volume);
        m_input->sampleVolume(context, *inputBuffer, volume);
        ScopedBuffer whenOutOfRangeBuffer = context.acquireBuffer(volume);
        m_whenOutOfRange->sampleVolume(context, *whenOutOfRangeBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            const float input = inputBuffer->get(i);
            if (!(input >= m_minInclusive) || !(input < m_maxExclusive)) {
                outputBuffer.set(i, whenOutOfRangeBuffer->get(i));
            }
        }
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const float inputValue = m_input->sampleValue(context, blockX, blockY, blockZ);
        return inputValue >= m_minInclusive && inputValue < m_maxExclusive
            ? m_whenInRange->sampleValue(context, blockX, blockY, blockZ)
            : m_whenOutOfRange->sampleValue(context, blockX, blockY, blockZ);
    }

private:
    DensitySamplerPtr m_input;
    float m_minInclusive;
    float m_maxExclusive;
    DensitySamplerPtr m_whenInRange;
    DensitySamplerPtr m_whenOutOfRange;
};

} // namespace

DensitySamplerPtr RangeChoiceFunction::compileSampler(CompileContext& context) const {
    DensitySamplerPtr input = m_input->compileSampler(context);
    if (const ConstantFunction* constantInRange = asConstant(m_whenInRange)) {
        const float whenInRangeValue = constantInRange->value();
        if (const ConstantFunction* constantOutOfRange = asConstant(m_whenOutOfRange)) {
            const float whenOutOfRangeValue = constantOutOfRange->value();
            return std::make_shared<RangeChoiceConstSampler>(input, m_minInclusive, m_maxExclusive, whenInRangeValue,
                                                  whenOutOfRangeValue);
        }
    }
    // Java compiles the arguments left to right.
    DensitySamplerPtr whenInRange = m_whenInRange->compileSampler(context);
    DensitySamplerPtr whenOutOfRange = m_whenOutOfRange->compileSampler(context);
    return std::make_shared<RangeChoiceSampler>(input, m_minInclusive, m_maxExclusive, whenInRange, whenOutOfRange);
}

DensityFunctionPtr RangeChoiceFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    DensityFunctionPtr whenInRange = rule.rewrite(m_whenInRange);
    DensityFunctionPtr whenOutOfRange = rule.rewrite(m_whenOutOfRange);
    return input == m_input && whenInRange == m_whenInRange && whenOutOfRange == m_whenOutOfRange
        ? self()
        : std::make_shared<RangeChoiceFunction>(input, m_minInclusive, m_maxExclusive, whenInRange, whenOutOfRange);
}

Interval RangeChoiceFunction::range() const {
    return Interval::encapsulating(std::vector<Interval>{m_whenInRange->range(), m_whenOutOfRange->range()});
}

int RangeChoiceFunction::domainAxes() const {
    return m_input->domainAxes() | m_whenInRange->domainAxes() | m_whenOutOfRange->domainAxes();
}

bool RangeChoiceFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const RangeChoiceFunction*>(&other);
    return o != nullptr && functionsEqual(m_input, o->m_input) && floatEq(m_minInclusive, o->m_minInclusive) &&
           floatEq(m_maxExclusive, o->m_maxExclusive) && functionsEqual(m_whenInRange, o->m_whenInRange) &&
           functionsEqual(m_whenOutOfRange, o->m_whenOutOfRange);
}

size_t RangeChoiceFunction::hash() const {
    size_t h = hashCombine(0x32, m_input->hash());
    h = hashCombine(h, hashFloat(m_minInclusive));
    h = hashCombine(h, hashFloat(m_maxExclusive));
    h = hashCombine(h, m_whenInRange->hash());
    return hashCombine(h, m_whenOutOfRange->hash());
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
