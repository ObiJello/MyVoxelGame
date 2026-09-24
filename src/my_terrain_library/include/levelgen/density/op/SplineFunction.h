#pragma once

#include "levelgen/density/BoundedFloatFunction.h"
#include "levelgen/density/CubicSpline.h"
#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <memory>
#include <string>

// Reference: op.SplineFunction (26.3) - "minecraft:spline". A CubicSpline whose
// coordinates are density functions (SplineFunction::Coordinate). Compiling
// maps each distinct coordinate function to one sampler, sampled at most once
// per point (and once per volume, lazily, on the volume path).

namespace minecraft {
namespace levelgen {
namespace density {

class SplineFunction final : public DensityFunction {
public:
    // SplineFunction.Coordinate: BoundedFloatFunction<Unit> over a density
    // function. apply() is never the real sampling path (it returns 0).
    class Coordinate final : public BoundedFloatFunction {
    public:
        explicit Coordinate(DensityFunctionPtr function) : m_function(std::move(function)) {}

        float apply(Argument&) const override { return 0.0f; }
        Interval range() const override { return m_function->range(); }
        bool equals(const BoundedFloatFunction& other) const override;
        size_t hash() const override { return hashCombine(0x41, m_function->hash()); }
        std::string toString() const override { return "Coordinate[function=" + m_function->typeId() + "]"; }

        const DensityFunctionPtr& function() const { return m_function; }

    private:
        DensityFunctionPtr m_function;
    };

    explicit SplineFunction(std::shared_ptr<const CubicSpline> spline) : m_spline(std::move(spline)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return m_spline->range(); }
    int domainAxes() const override;
    std::string typeId() const override { return "minecraft:spline"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x42, m_spline->hash()); }

    const std::shared_ptr<const CubicSpline>& spline() const { return m_spline; }

private:
    std::shared_ptr<const CubicSpline> m_spline;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
