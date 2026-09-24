#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: op.BlendDensityFunction (26.3) - "minecraft:blend_density".
// Passes its input through the context's Blender (blenderKey()), if any.

namespace minecraft {
namespace levelgen {
namespace density {

class BlendDensityFunction final : public DensityFunction {
public:
    explicit BlendDensityFunction(DensityFunctionPtr input) : m_input(std::move(input)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return m_input->range(); }
    int domainAxes() const override { return m_input->domainAxes(); }
    std::string typeId() const override { return "minecraft:blend_density"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x36, m_input->hash()); }

    const DensityFunctionPtr& input() const { return m_input; }

private:
    DensityFunctionPtr m_input;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
