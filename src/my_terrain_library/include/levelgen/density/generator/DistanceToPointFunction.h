#pragma once

#include "core/Vec3i.h"
#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/DistanceMetric.h"

#include <string>

// Reference: generator.DistanceToPointFunction (26.3), "minecraft:distance_to_point".

namespace minecraft {
namespace levelgen {
namespace density {

class DistanceToPointFunction final : public DensityFunction {
public:
    DistanceToPointFunction(core::Vec3i point, DistanceMetric metric) : m_point(point), m_metric(metric) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule&) const override { return self(); }
    Interval range() const override;
    int domainAxes() const override { return 7; }
    std::string typeId() const override { return "minecraft:distance_to_point"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const core::Vec3i& point() const { return m_point; }
    DistanceMetric metric() const { return m_metric; }

private:
    core::Vec3i m_point;
    DistanceMetric m_metric;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
