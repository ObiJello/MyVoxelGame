#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DensityFunctionCompiler.h"
#include "random/AnyPositionalRandomFactory.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
class BlockState;
}
}
}
}
}

// Reference: levelgen.Aquifer (26.3) - which fluid (or nothing) fills an
// empty cell of the terrain: the noise-based aquifers of the overworld
// (NoiseBasedAquifer) or a plain global fluid level (createDisabled).
// Densities come in as the double of the float final density, and the
// aquifer's own noises are sampled through the chunk's caching samplers.

namespace minecraft {
namespace levelgen {
namespace density {

using BlockState = ::minecraft::world::level::block::state::BlockState;

class Aquifer {
public:
    struct FluidStatus {
        int fluidLevel = 0;
        BlockState* fluidType = nullptr;

        // Blocks below the level are the fluid; the rest is air.
        BlockState* at(int blockY) const;
        bool operator==(const FluidStatus& other) const {
            return fluidLevel == other.fluidLevel && fluidType == other.fluidType;
        }
    };

    // Aquifer.FluidPicker.
    using FluidPicker = std::function<FluidStatus(int blockX, int blockY, int blockZ)>;

    // Aquifer.Config: the noise-settings "aquifers" object.
    struct Config {
        DensityFunctionPtr barrierNoise;
        DensityFunctionPtr fluidLevelFloodednessNoise;
        DensityFunctionPtr fluidLevelSpreadNoise;
        DensityFunctionPtr lavaNoise;
        DensityFunctionPtr exclusion;
        DensityFunctionPtr surfaceLevel;

        std::unique_ptr<Aquifer> create(const DensitySamplerSet& cachingSamplers,
                                        random::AnyPositionalRandomFactory positionalRandomFactory,
                                        const DensityVolume& volume, FluidPicker fluidRule) const;
    };

    virtual ~Aquifer() = default;

    // Null: solid (the caller places the default block).
    virtual BlockState* computeSubstance(int blockX, int blockY, int blockZ, double density) = 0;
    virtual bool shouldScheduleFluidUpdate() const = 0;

    static std::unique_ptr<Aquifer> createDisabled(FluidPicker fluidRule);

    // DimensionType.WAY_BELOW_MIN_Y.
    static constexpr int WAY_BELOW_MIN_Y = -2032 << 4;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
