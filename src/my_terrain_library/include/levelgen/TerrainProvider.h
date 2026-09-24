#pragma once

#include <cmath>

// Reference: data.worldgen.TerrainProvider (26.3). The overworld's terrain
// splines themselves are data (worldgen/density_function); only the
// peaks-and-valleys fold is still called from code (the F3 biome builder
// line and NoiseRouterData.peaksAndValleys).

namespace minecraft {
namespace levelgen {
namespace TerrainProvider {

// -(|(|weirdness| - 2/3)| - 1/3) * 3: valleys at 0, peaks at +-2/3.
inline float peaksAndValleys(float weirdness) {
    return -(std::fabs(std::fabs(weirdness) - 0.6666667f) - 0.33333334f) * 3.0f;
}

} // namespace TerrainProvider
} // namespace levelgen
} // namespace minecraft
