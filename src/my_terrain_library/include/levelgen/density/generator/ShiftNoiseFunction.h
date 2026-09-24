#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: generator.ShiftNoiseFunction (26.3) - the coordinate-offset
// noises: ShiftA "minecraft:shift_a" (x, 0, z), ShiftB "minecraft:shift_b"
// (z, x, 0) and Shift "minecraft:shift" (x, y, z), each sampled at a quarter
// of the block coordinate and scaled by 4.

namespace minecraft {
namespace levelgen {
namespace density {

class ShiftNoiseFunction : public DensityFunction {
public:
    static constexpr double COORDINATE_FACTOR = 0.25;
    static constexpr float VALUE_FACTOR = 4.0f;

    class ShiftA;
    class ShiftB;
    class Shift;

    explicit ShiftNoiseFunction(NoiseHolder offsetNoise) : m_offsetNoise(std::move(offsetNoise)) {}

    const NoiseHolder& offsetNoise() const { return m_offsetNoise; }

    // The interface's default range().
    Interval range() const override;

protected:
    NoiseHolder m_offsetNoise;
};

class ShiftNoiseFunction::ShiftA final : public ShiftNoiseFunction {
public:
    explicit ShiftA(NoiseHolder offsetNoise) : ShiftNoiseFunction(std::move(offsetNoise)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    int domainAxes() const override { return 5; }
    std::string typeId() const override { return "minecraft:shift_a"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x26, m_offsetNoise.hash()); }
};

class ShiftNoiseFunction::ShiftB final : public ShiftNoiseFunction {
public:
    explicit ShiftB(NoiseHolder offsetNoise) : ShiftNoiseFunction(std::move(offsetNoise)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    int domainAxes() const override { return 5; }
    std::string typeId() const override { return "minecraft:shift_b"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x27, m_offsetNoise.hash()); }
};

class ShiftNoiseFunction::Shift final : public ShiftNoiseFunction {
public:
    explicit Shift(NoiseHolder offsetNoise) : ShiftNoiseFunction(std::move(offsetNoise)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    int domainAxes() const override { return 7; }
    std::string typeId() const override { return "minecraft:shift"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x28, m_offsetNoise.hash()); }
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
