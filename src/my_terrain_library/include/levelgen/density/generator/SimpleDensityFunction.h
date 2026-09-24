#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: generator.SimpleDensityFunction (26.3) - the enum of
// context-bound leaves: blend_alpha, blend_offset, beardifier. Each value is
// one shared instance (blendAlpha()/blendOffset()/beardifier()), as a Java
// enum constant is.

namespace minecraft {
namespace levelgen {
namespace density {

class SimpleDensityFunction final : public DensityFunction {
public:
    enum class Kind {
        BLEND_ALPHA,
        BLEND_OFFSET,
        BEARDIFIER,
    };

    // Use the shared instances below; constructing another one is equal to
    // them but is not the same object.
    explicit SimpleDensityFunction(Kind kind) : m_kind(kind) {}

    static DensityFunctionPtr blendAlpha();
    static DensityFunctionPtr blendOffset();
    static DensityFunctionPtr beardifier();
    static DensityFunctionPtr of(Kind kind);

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule&) const override { return self(); }
    Interval range() const override;
    int domainAxes() const override;
    std::string typeId() const override { return std::string("minecraft:") + id(); }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x21, static_cast<size_t>(m_kind)); }

    const char* id() const;
    Kind kind() const { return m_kind; }

private:
    Kind m_kind;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
