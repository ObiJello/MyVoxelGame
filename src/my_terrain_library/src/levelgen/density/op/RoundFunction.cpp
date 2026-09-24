#include "levelgen/density/op/RoundFunction.h"

#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/JavaMath.h"

#include <cmath>
#include <stdexcept>

// Reference: op.RoundFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

const char* RoundFunction::typeName(Type type) {
    switch (type) {
        case Type::FLOOR: return "floor";
        case Type::ROUND: return "round";
        case Type::CEIL: return "ceil";
        case Type::TRUNCATE: return "truncate";
    }
    throw std::logic_error("RoundFunction: bad type");
}

float RoundFunction::roundToInteger(float input, Type type) {
    switch (type) {
        case Type::FLOOR: return static_cast<float>(std::floor(static_cast<double>(input)));
        // Math.round(float) returns an int.
        case Type::ROUND: return static_cast<float>(jmath::round(input));
        case Type::CEIL: return static_cast<float>(std::ceil(static_cast<double>(input)));
        case Type::TRUNCATE:
            return input > 0.0f ? static_cast<float>(std::floor(static_cast<double>(input)))
                                : static_cast<float>(std::ceil(static_cast<double>(input)));
    }
    throw std::logic_error("RoundFunction: bad type");
}

namespace {

class IntegerMultipleSampler final : public DensitySampler {
public:
    IntegerMultipleSampler(RoundFunction::Type type, DensitySamplerPtr input) : m_type(type), m_input(std::move(input)) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_input->sampleVolume(context, outputBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            outputBuffer.set(i, RoundFunction::roundToInteger(outputBuffer.get(i), m_type));
        }
    }
    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const float input = m_input->sampleValue(context, blockX, blockY, blockZ);
        return RoundFunction::roundToInteger(input, m_type);
    }
private:
    RoundFunction::Type m_type;
    DensitySamplerPtr m_input;
};

class Sampler final : public DensitySampler {
public:
    Sampler(RoundFunction::Type type, DensitySamplerPtr input, DensitySamplerPtr multiple)
        : m_type(type), m_input(std::move(input)), m_multiple(std::move(multiple)) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_input->sampleVolume(context, outputBuffer, volume);
        ScopedBuffer multipleBuffer = context.acquireBuffer(volume);
        m_multiple->sampleVolume(context, *multipleBuffer, volume);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            const float input = outputBuffer.get(i);
            const float multiple = multipleBuffer->get(i);
            outputBuffer.set(i, apply(input, multiple));
        }
    }
    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const float input = m_input->sampleValue(context, blockX, blockY, blockZ);
        const float multiple = m_multiple->sampleValue(context, blockX, blockY, blockZ);
        return apply(input, multiple);
    }
private:
    float apply(float input, float multiple) const {
        return multiple == 0.0f ? input : RoundFunction::roundToInteger(input / multiple, m_type) * multiple;
    }

    RoundFunction::Type m_type;
    DensitySamplerPtr m_input;
    DensitySamplerPtr m_multiple;
};

} // namespace

DensitySamplerPtr RoundFunction::compileSampler(CompileContext& context) const {
    DensitySamplerPtr input = m_input->compileSampler(context);
    if (const ConstantFunction* c = asConstant(m_multiple)) {
        const float multipleValue = c->value();
        if (multipleValue == 1.0f) {
            return std::make_shared<IntegerMultipleSampler>(m_type, input);
        }
    }
    return std::make_shared<Sampler>(m_type, input, m_multiple->compileSampler(context));
}

DensityFunctionPtr RoundFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    DensityFunctionPtr multiple = rule.rewrite(m_multiple);
    return input == m_input && multiple == m_multiple ? self()
                                                      : std::make_shared<RoundFunction>(m_type, input, multiple);
}

Interval RoundFunction::range() const {
    const Interval multipleRange = m_multiple->range();
    const Type type = m_type;
    return Interval::mul(Interval::mapMonotonic(Interval::div(m_input->range(), multipleRange),
                                                [type](float value) { return roundToInteger(value, type); }),
                         multipleRange);
}

bool RoundFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const RoundFunction*>(&other);
    return o != nullptr && m_type == o->m_type && functionsEqual(m_input, o->m_input) &&
           functionsEqual(m_multiple, o->m_multiple);
}

size_t RoundFunction::hash() const {
    size_t h = hashCombine(0x35, static_cast<size_t>(m_type));
    h = hashCombine(h, m_input->hash());
    return hashCombine(h, m_multiple->hash());
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
