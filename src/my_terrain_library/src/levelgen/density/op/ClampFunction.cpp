#include "levelgen/density/op/ClampFunction.h"

// Reference: op.ClampFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

DensitySamplerPtr ClampFunction::compileSampler(CompileContext& context) const {
    return std::make_shared<Sampler>(m_input->compileSampler(context), m_min, m_max);
}

DensityFunctionPtr ClampFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    return input == m_input ? self() : std::make_shared<ClampFunction>(input, m_min, m_max);
}

bool ClampFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const ClampFunction*>(&other);
    return o != nullptr && functionsEqual(m_input, o->m_input) && floatEq(m_min, o->m_min) &&
           floatEq(m_max, o->m_max);
}

size_t ClampFunction::hash() const {
    size_t h = hashCombine(0x33, m_input->hash());
    h = hashCombine(h, hashFloat(m_min));
    return hashCombine(h, hashFloat(m_max));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
