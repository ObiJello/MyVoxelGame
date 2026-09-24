#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: op.InterpolatedFunction (26.3) - "minecraft:interpolated".
// Samples its input on the cell grid and trilinearly interpolates between.

namespace minecraft {
namespace levelgen {
namespace density {

class InterpolatedFunction final : public DensityFunction {
public:
    InterpolatedFunction(DensityFunctionPtr input, int cellSizeXz, int cellSizeY)
        : m_input(std::move(input)), m_cellSizeXz(cellSizeXz), m_cellSizeY(cellSizeY) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return m_input->range(); }
    int domainAxes() const override { return m_input->domainAxes(); }
    std::string typeId() const override { return "minecraft:interpolated"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const DensityFunctionPtr& input() const { return m_input; }
    int cellSizeXz() const { return m_cellSizeXz; }
    int cellSizeY() const { return m_cellSizeY; }

private:
    DensityFunctionPtr m_input;
    int m_cellSizeXz;
    int m_cellSizeY;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
