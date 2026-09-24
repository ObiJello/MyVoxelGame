#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: op.RangeChoiceFunction (26.3) - "minecraft:range_choice".

namespace minecraft {
namespace levelgen {
namespace density {

class RangeChoiceFunction final : public DensityFunction {
public:
    RangeChoiceFunction(DensityFunctionPtr input, float minInclusive, float maxExclusive,
                        DensityFunctionPtr whenInRange, DensityFunctionPtr whenOutOfRange)
        : m_input(std::move(input)), m_minInclusive(minInclusive), m_maxExclusive(maxExclusive),
          m_whenInRange(std::move(whenInRange)), m_whenOutOfRange(std::move(whenOutOfRange)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override;
    std::string typeId() const override { return "minecraft:range_choice"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const DensityFunctionPtr& input() const { return m_input; }
    float minInclusive() const { return m_minInclusive; }
    float maxExclusive() const { return m_maxExclusive; }
    const DensityFunctionPtr& whenInRange() const { return m_whenInRange; }
    const DensityFunctionPtr& whenOutOfRange() const { return m_whenOutOfRange; }

private:
    DensityFunctionPtr m_input;
    float m_minInclusive;
    float m_maxExclusive;
    DensityFunctionPtr m_whenInRange;
    DensityFunctionPtr m_whenOutOfRange;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
