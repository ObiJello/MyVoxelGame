#include "levelgen/density/op/IntervalSelectFunction.h"

#include "levelgen/density/SamplerContext.h"

// Reference: op.IntervalSelectFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// IntervalSelectFunction.SingleThresholdSampler.
class IntervalSelectSingleThresholdSampler final : public DensitySampler {
public:
    IntervalSelectSingleThresholdSampler(DensitySamplerPtr input, float threshold, DensitySamplerPtr ifBelow,
                                         DensitySamplerPtr ifAbove)
        : m_input(std::move(input)), m_threshold(threshold), m_ifBelow(std::move(ifBelow)),
          m_ifAbove(std::move(ifAbove)) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_input->sampleVolume(context, outputBuffer, volume);
        ScopedBuffer ifBelowBuffer = context.acquireBuffer(volume);
        m_ifBelow->sampleVolume(context, *ifBelowBuffer, volume);
        ScopedBuffer ifAboveBuffer = context.acquireBuffer(volume);
        m_ifAbove->sampleVolume(context, *ifAboveBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            const float input = outputBuffer.get(i);
            outputBuffer.set(i, input < m_threshold ? ifBelowBuffer->get(i) : ifAboveBuffer->get(i));
        }
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const float input = m_input->sampleValue(context, blockX, blockY, blockZ);
        return input < m_threshold ? m_ifBelow->sampleValue(context, blockX, blockY, blockZ)
                                   : m_ifAbove->sampleValue(context, blockX, blockY, blockZ);
    }

private:
    DensitySamplerPtr m_input;
    float m_threshold;
    DensitySamplerPtr m_ifBelow;
    DensitySamplerPtr m_ifAbove;
};

// IntervalSelectFunction.Sampler.
class IntervalSelectSampler final : public DensitySampler {
public:
    IntervalSelectSampler(DensitySamplerPtr input, std::vector<float> thresholds,
                          std::vector<DensitySamplerPtr> samplers)
        : m_input(std::move(input)), m_thresholds(std::move(thresholds)), m_samplers(std::move(samplers)) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_input->sampleVolume(context, outputBuffer, volume);
        const int samplerCount = static_cast<int>(m_samplers.size());
        std::vector<ScopedBuffer> buffers(static_cast<size_t>(samplerCount));
        for (int i = 0; i < samplerCount; ++i) {
            buffers[static_cast<size_t>(i)] = context.acquireBuffer(volume);
            m_samplers[static_cast<size_t>(i)]->sampleVolume(context, *buffers[static_cast<size_t>(i)], volume);
        }
        for (int i = 0; i < outputBuffer.size(); ++i) {
            const int samplerIndex = selectSamplerIndex(outputBuffer.get(i));
            outputBuffer.set(i, buffers[static_cast<size_t>(samplerIndex)]->get(i));
        }
        // Java closes the buffers in index order.
        for (ScopedBuffer& buffer : buffers) {
            buffer.reset();
        }
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const float input = m_input->sampleValue(context, blockX, blockY, blockZ);
        return m_samplers[static_cast<size_t>(selectSamplerIndex(input))]->sampleValue(context, blockX, blockY, blockZ);
    }

private:
    int selectSamplerIndex(float input) const {
        for (int i = 0; i < static_cast<int>(m_thresholds.size()); ++i) {
            if (input < m_thresholds[static_cast<size_t>(i)]) {
                return i;
            }
        }
        return static_cast<int>(m_samplers.size()) - 1;
    }

    DensitySamplerPtr m_input;
    std::vector<float> m_thresholds;
    std::vector<DensitySamplerPtr> m_samplers;
};

} // namespace

DensitySamplerPtr IntervalSelectFunction::compileSampler(CompileContext& context) const {
    DensitySamplerPtr input = m_input->compileSampler(context);
    if (m_thresholds.size() == 1) {
        const float threshold = m_thresholds[0];
        DensitySamplerPtr ifBelow = m_functions.front()->compileSampler(context);
        DensitySamplerPtr ifAbove = m_functions.back()->compileSampler(context);
        return std::make_shared<IntervalSelectSingleThresholdSampler>(input, threshold, ifBelow, ifAbove);
    }
    std::vector<DensitySamplerPtr> samplers;
    samplers.reserve(m_functions.size());
    for (const DensityFunctionPtr& function : m_functions) {
        samplers.push_back(function->compileSampler(context));
    }
    return std::make_shared<IntervalSelectSampler>(input, m_thresholds, std::move(samplers));
}

DensityFunctionPtr IntervalSelectFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    bool functionsChanged = false;
    std::vector<DensityFunctionPtr> functions;
    functions.reserve(m_functions.size());
    for (const DensityFunctionPtr& function : m_functions) {
        DensityFunctionPtr newFunction = rule.rewrite(function);
        if (newFunction != function) {
            functionsChanged = true;
        }
        functions.push_back(std::move(newFunction));
    }
    return input == m_input && !functionsChanged
        ? self()
        : std::make_shared<IntervalSelectFunction>(input, m_thresholds, std::move(functions));
}

Interval IntervalSelectFunction::range() const {
    std::vector<Interval> ranges;
    ranges.reserve(m_functions.size());
    for (const DensityFunctionPtr& function : m_functions) {
        ranges.push_back(function->range());
    }
    return Interval::encapsulating(ranges);
}

int IntervalSelectFunction::domainAxes() const {
    int axes = m_input->domainAxes();
    for (const DensityFunctionPtr& function : m_functions) {
        axes |= function->domainAxes();
    }
    return axes;
}

bool IntervalSelectFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const IntervalSelectFunction*>(&other);
    if (o == nullptr || !functionsEqual(m_input, o->m_input)) return false;
    // FloatArrayList.equals compares elements with the primitive `!=`.
    if (m_thresholds.size() != o->m_thresholds.size()) return false;
    for (size_t i = 0; i < m_thresholds.size(); ++i) {
        if (m_thresholds[i] != o->m_thresholds[i]) return false;
    }
    if (m_functions.size() != o->m_functions.size()) return false;
    for (size_t i = 0; i < m_functions.size(); ++i) {
        if (!functionsEqual(m_functions[i], o->m_functions[i])) return false;
    }
    return true;
}

size_t IntervalSelectFunction::hash() const {
    size_t h = hashCombine(0x33, m_input->hash());
    for (float threshold : m_thresholds) {
        // +0.0f so -0.0f and 0.0f (equal under `!=`) hash alike.
        h = hashCombine(h, hashFloat(threshold + 0.0f));
    }
    for (const DensityFunctionPtr& function : m_functions) {
        h = hashCombine(h, function->hash());
    }
    return h;
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
