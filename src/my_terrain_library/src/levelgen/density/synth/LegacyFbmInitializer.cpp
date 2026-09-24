#include "levelgen/density/synth/LegacyFbmInitializer.h"

#include "levelgen/density/synth/PerlinNoise.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>

// Reference: synth.LegacyFbmInitializer (26.3).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

NoiseStackPtr LegacyFbmInitializer::createForLegacyNetherBiome(random::AnyRandomSource& random, int firstOctave,
                                                               const std::vector<double>& amplitudes) {
    const int octaves = static_cast<int>(amplitudes.size());
    const int zeroOctaveIndex = -firstOctave;
    std::vector<std::shared_ptr<const PerlinNoise>> noiseLevels(static_cast<size_t>(octaves));
    std::shared_ptr<const PerlinNoise> zeroOctave = std::make_shared<const PerlinNoise>(random);
    double factor;
    if (zeroOctaveIndex >= 0 && zeroOctaveIndex < octaves) {
        factor = amplitudes[static_cast<size_t>(zeroOctaveIndex)];
        if (factor != 0.0) {
            noiseLevels[static_cast<size_t>(zeroOctaveIndex)] = zeroOctave;
        }
    }

    for (int i = zeroOctaveIndex - 1; i >= 0; --i) {
        if (i < octaves) {
            const double amplitude = amplitudes[static_cast<size_t>(i)];
            if (amplitude != 0.0) {
                noiseLevels[static_cast<size_t>(i)] = std::make_shared<const PerlinNoise>(random);
            } else {
                skipOctave(random);
            }
        } else {
            skipOctave(random);
        }
    }

    int64_t createdLevels = 0;
    for (const auto& level : noiseLevels) {
        if (level != nullptr) ++createdLevels;
    }
    int64_t nonZeroAmplitudes = 0;
    for (double amplitude : amplitudes) {
        if (amplitude != 0.0) ++nonZeroAmplitudes;
    }
    if (createdLevels != nonZeroAmplitudes) {
        throw std::logic_error("Failed to create correct number of noise levels for given non-zero amplitudes");
    }
    if (zeroOctaveIndex < octaves - 1) {
        throw std::invalid_argument("Positive octaves are temporarily disabled");
    }

    factor = std::pow(2.0, static_cast<double>(-zeroOctaveIndex));
    double valueFactor = std::pow(2.0, static_cast<double>(octaves - 1)) /
                         (std::pow(2.0, static_cast<double>(octaves)) - 1.0);
    NoiseStack::Builder stack = NoiseStack::builder();

    for (size_t i = 0; i < noiseLevels.size(); ++i) {
        const std::shared_ptr<const PerlinNoise>& noise = noiseLevels[i];
        if (noise != nullptr) {
            stack.add(noise, factor, static_cast<float>(valueFactor * amplitudes[i]));
        }
        factor *= 2.0;
        valueFactor /= 2.0;
    }
    return stack.build();
}

void LegacyFbmInitializer::skipOctave(random::AnyRandomSource& random) {
    random.consumeCount(262);
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
