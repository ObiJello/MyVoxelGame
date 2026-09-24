#include "levelgen/density/op/BinaryFunction.h"

#include "levelgen/density/CoreFunctions.h"

#include <cstdio>
#include <stdexcept>

// Reference: op.BinaryFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

const char* BinaryFunction::typeName(Type type) {
    switch (type) {
        case Type::ADD: return "add";
        case Type::SUB: return "sub";
        case Type::MUL: return "mul";
        case Type::DIV: return "div";
        case Type::MIN: return "min";
        case Type::MAX: return "max";
    }
    throw std::logic_error("BinaryFunction: bad type");
}

DensitySamplerPtr BinaryFunction::compileSampler(CompileContext& context) const {
    DensitySamplerPtr left = m_left->compileSampler(context);
    DensitySamplerPtr right = m_right->compileSampler(context);
    switch (m_type) {
        case Type::ADD: {
            if (const ConstantFunction* c = asConstant(m_left)) {
                return std::make_shared<ConstAddSampler>(right, c->value());
            }
            if (const ConstantFunction* c = asConstant(m_right)) {
                return std::make_shared<ConstAddSampler>(left, c->value());
            }
            return std::make_shared<AddSampler>(left, right);
        }
        case Type::SUB: {
            if (const ConstantFunction* c = asConstant(m_left)) {
                return std::make_shared<ConstSubSampler>(c->value(), right);
            }
            if (const ConstantFunction* c = asConstant(m_right)) {
                return std::make_shared<ConstAddSampler>(left, -c->value());
            }
            return std::make_shared<SubSampler>(left, right);
        }
        case Type::MUL: {
            if (const ConstantFunction* c = asConstant(m_left)) {
                return std::make_shared<ConstMulSampler>(right, c->value());
            }
            if (const ConstantFunction* c = asConstant(m_right)) {
                return std::make_shared<ConstMulSampler>(left, c->value());
            }
            return std::make_shared<MulSampler>(left, right);
        }
        case Type::DIV: {
            if (const ConstantFunction* c = asConstant(m_left)) {
                return std::make_shared<ConstDivSampler>(c->value(), right);
            }
            if (const ConstantFunction* c = asConstant(m_right)) {
                return std::make_shared<ConstMulSampler>(left, 1.0f / c->value());
            }
            return std::make_shared<DivSampler>(left, right);
        }
        case Type::MIN: {
            const Interval leftRange = m_left->range();
            const Interval rightRange = m_right->range();
            if (leftRange.max() < rightRange.min()) {
                warnNonIntersecting();
                return left;
            }
            if (rightRange.max() < leftRange.min()) {
                warnNonIntersecting();
                return right;
            }
            if (const ConstantFunction* c = asConstant(m_left)) {
                return std::make_shared<ConstMinSampler>(right, c->value());
            }
            if (const ConstantFunction* c = asConstant(m_right)) {
                return std::make_shared<ConstMinSampler>(left, c->value());
            }
            return std::make_shared<MinSampler>(left, right, rightRange.min());
        }
        case Type::MAX: {
            const Interval leftRange = m_left->range();
            const Interval rightRange = m_right->range();
            if (leftRange.min() > rightRange.max()) {
                warnNonIntersecting();
                return left;
            }
            if (rightRange.min() > leftRange.max()) {
                warnNonIntersecting();
                return right;
            }
            if (const ConstantFunction* c = asConstant(m_left)) {
                return std::make_shared<ConstMaxSampler>(right, c->value());
            }
            if (const ConstantFunction* c = asConstant(m_right)) {
                return std::make_shared<ConstMaxSampler>(left, c->value());
            }
            return std::make_shared<MaxSampler>(left, right, rightRange.max());
        }
    }
    throw std::logic_error("BinaryFunction: bad type");
}

void BinaryFunction::warnNonIntersecting() const {
    std::fprintf(stderr,
                 "[BinaryFunction] Compiling a %s function between two non-overlapping inputs: %s (%s) and %s (%s)\n",
                 typeName(m_type), m_left->typeId().c_str(), m_left->range().toString().c_str(),
                 m_right->typeId().c_str(), m_right->range().toString().c_str());
}

DensityFunctionPtr BinaryFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr left = rule.rewrite(m_left);
    DensityFunctionPtr right = rule.rewrite(m_right);
    return left == m_left && m_right == right ? self() : std::make_shared<BinaryFunction>(m_type, left, right);
}

Interval BinaryFunction::range() const {
    const Interval left = m_left->range();
    const Interval right = m_right->range();
    switch (m_type) {
        case Type::ADD: return Interval::add(left, right);
        case Type::SUB: return Interval::sub(left, right);
        case Type::MUL: return Interval::mul(left, right);
        case Type::DIV: return Interval::div(left, right);
        case Type::MIN: return Interval::min(left, right);
        case Type::MAX: return Interval::max(left, right);
    }
    throw std::logic_error("BinaryFunction: bad type");
}

bool BinaryFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const BinaryFunction*>(&other);
    return o != nullptr && m_type == o->m_type && functionsEqual(m_left, o->m_left) &&
           functionsEqual(m_right, o->m_right);
}

size_t BinaryFunction::hash() const {
    size_t h = hashCombine(0x32, static_cast<size_t>(m_type));
    h = hashCombine(h, m_left->hash());
    return hashCombine(h, m_right->hash());
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
