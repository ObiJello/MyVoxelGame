#pragma once

#include "levelgen/density/synth/NoiseStack.h"
#include "random/AnyPositionalRandomFactory.h"

#include <vector>

// Reference: synth.LegacyFbmInitializer (26.3, deprecated) - the pre-1.18
// octave layout the Nether biome noises (temperature/vegetation) still use:
// octaves are drawn from ONE sequential random, from octave 0 downwards,
// skipping (262 draws) the ones whose amplitude is zero.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class LegacyFbmInitializer {
public:
    // Throws std::logic_error (IllegalStateException) when the octave count
    // does not match, std::invalid_argument for positive octaves.
    static NoiseStackPtr createForLegacyNetherBiome(random::AnyRandomSource& random, int firstOctave,
                                                    const std::vector<double>& amplitudes);

private:
    static void skipOctave(random::AnyRandomSource& random);
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
