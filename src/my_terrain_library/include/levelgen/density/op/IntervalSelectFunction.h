#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>
#include <vector>

// Reference: op.IntervalSelectFunction (26.3) - "minecraft:interval_select".
// thresholds.size() == functions.size() - 1, ascending (the codec validates;
// the loader must check before constructing).

namespace minecraft {
namespace levelgen {
namespace density {

class IntervalSelectFunction final : public DensityFunction {
public:
    IntervalSelectFunction(DensityFunctionPtr input, std::vector<float> thresholds,
                           std::vector<DensityFunctionPtr> functions)
        : m_input(std::move(input)), m_thresholds(std::move(thresholds)), m_functions(std::move(functions)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override;
    std::string typeId() const override { return "minecraft:interval_select"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    const DensityFunctionPtr& input() const { return m_input; }
    const std::vector<float>& thresholds() const { return m_thresholds; }
    const std::vector<DensityFunctionPtr>& functions() const { return m_functions; }

private:
    DensityFunctionPtr m_input;
    std::vector<float> m_thresholds;
    std::vector<DensityFunctionPtr> m_functions;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
