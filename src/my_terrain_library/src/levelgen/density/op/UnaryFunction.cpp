#include "levelgen/density/op/UnaryFunction.h"

#include <stdexcept>

// Reference: op.UnaryFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

const char* UnaryFunction::typeName(Type type) {
    switch (type) {
        case Type::ABS: return "abs";
        case Type::SQUARE: return "square";
        case Type::CUBE: return "cube";
        case Type::SQRT: return "sqrt";
        case Type::HALF_NEGATIVE: return "half_negative";
        case Type::QUARTER_NEGATIVE: return "quarter_negative";
        case Type::RECIPROCAL: return "reciprocal";
        case Type::NEGATE: return "negate";
        case Type::SQUEEZE: return "squeeze";
        case Type::LOG: return "log";
        case Type::SIGN: return "sign";
    }
    throw std::logic_error("UnaryFunction: bad type");
}

DensitySamplerPtr UnaryFunction::compileSampler(CompileContext& context) const {
    DensitySamplerPtr input = m_input->compileSampler(context);
    switch (m_type) {
        case Type::ABS: return std::make_shared<AbsSampler>(input);
        case Type::SQUARE: return std::make_shared<SquareSampler>(input);
        case Type::CUBE: return std::make_shared<CubeSampler>(input);
        case Type::SQRT: return std::make_shared<SqrtSampler>(input);
        case Type::HALF_NEGATIVE: return std::make_shared<LeakyReLUSampler>(input, 0.5f);
        case Type::QUARTER_NEGATIVE: return std::make_shared<LeakyReLUSampler>(input, 0.25f);
        case Type::RECIPROCAL: return std::make_shared<ReciprocalSampler>(input);
        case Type::NEGATE: return std::make_shared<NegateSampler>(input);
        case Type::SQUEEZE: return std::make_shared<SqueezeSampler>(input);
        case Type::LOG: return std::make_shared<LogSampler>(input);
        case Type::SIGN: return std::make_shared<SignSampler>(input);
    }
    throw std::logic_error("UnaryFunction: bad type");
}

DensityFunctionPtr UnaryFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    return input == m_input ? self() : std::make_shared<UnaryFunction>(m_type, input);
}

Interval UnaryFunction::range() const {
    const Interval input = m_input->range();
    switch (m_type) {
        case Type::ABS: return Interval::abs(input);
        case Type::SQUARE: return Interval::square(input);
        case Type::CUBE: return Interval::mapMonotonic(input, [](float value) { return jmath::cube(value); });
        case Type::SQRT: return Interval::pow(input, Interval::ofExact(0.5f));
        case Type::HALF_NEGATIVE:
            return Interval::mapMonotonic(input, [](float value) { return LeakyReLUSampler::apply(0.5f, value); });
        case Type::QUARTER_NEGATIVE:
            return Interval::mapMonotonic(input, [](float value) { return LeakyReLUSampler::apply(0.25f, value); });
        case Type::RECIPROCAL: return Interval::reciprocal(input);
        case Type::NEGATE: return Interval::sub(Interval::ofExact(0.0f), input);
        case Type::SQUEEZE: return Interval::mapMonotonic(input, [](float value) { return SqueezeSampler::apply(value); });
        case Type::LOG: return Interval::log(input);
        case Type::SIGN: return Interval::sign(input);
    }
    throw std::logic_error("UnaryFunction: bad type");
}

bool UnaryFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const UnaryFunction*>(&other);
    return o != nullptr && m_type == o->m_type && functionsEqual(m_input, o->m_input);
}

size_t UnaryFunction::hash() const {
    size_t h = hashCombine(0x31, static_cast<size_t>(m_type));
    return hashCombine(h, m_input->hash());
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
