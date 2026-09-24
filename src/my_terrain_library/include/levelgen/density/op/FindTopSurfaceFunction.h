#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: op.FindTopSurfaceFunction (26.3) - "minecraft:find_top_surface".
// The highest multiple of cellHeight, from upperBound down to lowerBound, at
// which density > 0 (independent of Y).

namespace minecraft {
namespace levelgen {
namespace density {

class FindTopSurfaceFunction final : public DensityFunction {
public:
    FindTopSurfaceFunction(DensityFunctionPtr density, DensityFunctionPtr upperBound, int lowerBound, int cellHeight)
        : m_density(std::move(density)), m_upperBound(std::move(upperBound)), m_lowerBound(lowerBound),
          m_cellHeight(cellHeight) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override;
    std::string typeId() const override { return "minecraft:find_top_surface"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const DensityFunctionPtr& density() const { return m_density; }
    const DensityFunctionPtr& upperBound() const { return m_upperBound; }
    int lowerBound() const { return m_lowerBound; }
    int cellHeight() const { return m_cellHeight; }

private:
    DensityFunctionPtr m_density;
    DensityFunctionPtr m_upperBound;
    int m_lowerBound;
    int m_cellHeight;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
