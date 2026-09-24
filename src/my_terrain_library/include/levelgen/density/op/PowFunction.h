#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: op.PowFunction (26.3), "minecraft:pow". A constant exponent of
// +-0.5, +-1, +-2 or +-3 compiles to the unary sqrt/identity/square/cube
// samplers (under a reciprocal when negative); anything else goes through
// Math.pow in double.

namespace minecraft {
namespace levelgen {
namespace density {

class PowFunction final : public DensityFunction {
public:
    PowFunction(DensityFunctionPtr base, DensityFunctionPtr exponent)
        : m_base(std::move(base)), m_exponent(std::move(exponent)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return Interval::pow(m_base->range(), m_exponent->range()); }
    int domainAxes() const override { return m_base->domainAxes() | m_exponent->domainAxes(); }
    std::string typeId() const override { return "minecraft:pow"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const DensityFunctionPtr& base() const { return m_base; }
    const DensityFunctionPtr& exponent() const { return m_exponent; }

private:
    static DensitySamplerPtr compileConstExponent(const DensitySamplerPtr& base, float exponent);

    DensityFunctionPtr m_base;
    DensityFunctionPtr m_exponent;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
