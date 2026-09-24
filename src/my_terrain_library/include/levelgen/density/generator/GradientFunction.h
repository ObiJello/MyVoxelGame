#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/TilingMode.h"

#include <string>

// Reference: generator.GradientFunction (26.3), "minecraft:gradient" - a
// linear ramp along one axis, clamped, repeated or mirror-repeated past its
// ends (DensityFunctions.yClampedGradient is the clamped Y case).

namespace minecraft {
namespace levelgen {
namespace density {

class GradientFunction final : public DensityFunction {
public:
    GradientFunction(Axis axis, TilingMode tiling, int fromCoordinate, int toCoordinate, float fromValue, float toValue)
        : m_axis(axis), m_tiling(tiling), m_fromCoordinate(fromCoordinate), m_toCoordinate(toCoordinate),
          m_fromValue(fromValue), m_toValue(toValue) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule&) const override { return self(); }
    Interval range() const override { return Interval::encapsulating(m_fromValue, m_toValue); }
    int domainAxes() const override { return axesFrom(m_axis); }
    std::string typeId() const override { return "minecraft:gradient"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    Axis axis() const { return m_axis; }
    TilingMode tiling() const { return m_tiling; }
    int fromCoordinate() const { return m_fromCoordinate; }
    int toCoordinate() const { return m_toCoordinate; }
    float fromValue() const { return m_fromValue; }
    float toValue() const { return m_toValue; }

private:
    Axis m_axis;
    TilingMode m_tiling;
    int m_fromCoordinate;
    int m_toCoordinate;
    float m_fromValue;
    float m_toValue;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
