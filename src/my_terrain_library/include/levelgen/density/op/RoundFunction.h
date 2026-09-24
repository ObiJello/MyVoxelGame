#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: op.RoundFunction (26.3) - floor, round, ceil, truncate to a
// multiple (default 1).

namespace minecraft {
namespace levelgen {
namespace density {

class RoundFunction final : public DensityFunction {
public:
    enum class Type {
        FLOOR,
        ROUND,
        CEIL,
        TRUNCATE,
    };
    static const char* typeName(Type type);

    RoundFunction(Type type, DensityFunctionPtr input, DensityFunctionPtr multiple)
        : m_type(type), m_input(std::move(input)), m_multiple(std::move(multiple)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override { return m_input->domainAxes() | m_multiple->domainAxes(); }
    std::string typeId() const override { return std::string("minecraft:") + typeName(m_type); }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    Type type() const { return m_type; }
    const DensityFunctionPtr& input() const { return m_input; }
    const DensityFunctionPtr& multiple() const { return m_multiple; }

    static float roundToInteger(float input, Type type);

private:
    Type m_type;
    DensityFunctionPtr m_input;
    DensityFunctionPtr m_multiple;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
