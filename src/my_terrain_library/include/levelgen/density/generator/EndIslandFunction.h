#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"

#include <string>

// Reference: generator.EndIslandFunction (26.3), "minecraft:end_outer_islands".

namespace minecraft {
namespace levelgen {
namespace density {

namespace synth {
class SimplexNoise;
}

class EndIslandFunction final : public DensityFunction {
public:
    EndIslandFunction() = default;

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule&) const override { return self(); }
    Interval range() const override { return Interval::of(-0.84375f, 0.5625f); }
    int domainAxes() const override { return 5; }
    std::string typeId() const override { return "minecraft:end_outer_islands"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return 0x23; }

    static float getHeightValue(const synth::SimplexNoise& islandNoise, int sectionX, int sectionZ);

private:
    static constexpr float ISLAND_THRESHOLD = -0.9f;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
