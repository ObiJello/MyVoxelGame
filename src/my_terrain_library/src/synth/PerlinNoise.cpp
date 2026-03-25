#include "synth/PerlinNoise.h"
#include "math/Mth.h"
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace minecraft {

PerlinNoise PerlinNoise::create(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes) {
    // Reference: PerlinNoise.java lines 57-59
    return PerlinNoise(random, firstOctave, amplitudes, true);
}

PerlinNoise PerlinNoise::create(LegacyRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes) {
    return PerlinNoise(random, firstOctave, amplitudes, true);
}

PerlinNoise PerlinNoise::create(levelgen::WorldgenRandom& random, int32_t firstOctave, const std::vector<double>& amplitudes) {
    return random.usesLegacyRandomSource()
        ? PerlinNoise::create(random.getLegacyRandomSource(), firstOctave, amplitudes)
        : PerlinNoise::create(random.getRandomSource(), firstOctave, amplitudes);
}

PerlinNoise PerlinNoise::create(XoroshiroRandomSource& random, int32_t firstOctave, double firstAmplitude, const std::vector<double>& additionalAmplitudes) {
    // Reference: PerlinNoise.java lines 51-55
    std::vector<double> amplitudes;
    amplitudes.push_back(firstAmplitude);
    amplitudes.insert(amplitudes.end(), additionalAmplitudes.begin(), additionalAmplitudes.end());
    return PerlinNoise(random, firstOctave, amplitudes, true);
}

PerlinNoise PerlinNoise::create(LegacyRandomSource& random, int32_t firstOctave, double firstAmplitude, const std::vector<double>& additionalAmplitudes) {
    std::vector<double> amplitudes;
    amplitudes.push_back(firstAmplitude);
    amplitudes.insert(amplitudes.end(), additionalAmplitudes.begin(), additionalAmplitudes.end());
    return PerlinNoise(random, firstOctave, amplitudes, true);
}

PerlinNoise PerlinNoise::create(levelgen::WorldgenRandom& random, int32_t firstOctave, double firstAmplitude, const std::vector<double>& additionalAmplitudes) {
    std::vector<double> amplitudes;
    amplitudes.push_back(firstAmplitude);
    amplitudes.insert(amplitudes.end(), additionalAmplitudes.begin(), additionalAmplitudes.end());
    return create(random, firstOctave, amplitudes);
}

PerlinNoise PerlinNoise::createLegacyForLegacyNetherBiome(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes) {
    // Reference: PerlinNoise.java lines 39-41
    return PerlinNoise(random, firstOctave, amplitudes, false);
}

PerlinNoise PerlinNoise::createLegacyForLegacyNetherBiome(LegacyRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes) {
    return PerlinNoise(random, firstOctave, amplitudes, false);
}

PerlinNoise PerlinNoise::createLegacyForLegacyNetherBiome(levelgen::WorldgenRandom& random, int32_t firstOctave, const std::vector<double>& amplitudes) {
    return random.usesLegacyRandomSource()
        ? PerlinNoise::createLegacyForLegacyNetherBiome(random.getLegacyRandomSource(), firstOctave, amplitudes)
        : PerlinNoise::createLegacyForLegacyNetherBiome(random.getRandomSource(), firstOctave, amplitudes);
}

PerlinNoise PerlinNoise::createLegacyForBlendedNoise(XoroshiroRandomSource& random, int32_t octaveStart, int32_t octaveEnd) {
    // Reference: PerlinNoise.java lines 33-35 and makeAmplitudes (lines 61-82)
    // Creates PerlinNoise with octaves from octaveStart to octaveEnd (inclusive)
    // All amplitudes are set to 1.0

    if (octaveStart > octaveEnd) {
        throw std::invalid_argument("octaveStart must be <= octaveEnd");
    }

    int32_t numOctaves = octaveEnd - octaveStart + 1;
    std::vector<double> amplitudes(numOctaves, 1.0);

    // firstOctave is the negation of octaveStart
    return PerlinNoise(random, octaveStart, amplitudes, false);
}

PerlinNoise PerlinNoise::createLegacyForBlendedNoise(LegacyRandomSource& random, int32_t octaveStart, int32_t octaveEnd) {
    if (octaveStart > octaveEnd) {
        throw std::invalid_argument("octaveStart must be <= octaveEnd");
    }

    int32_t numOctaves = octaveEnd - octaveStart + 1;
    std::vector<double> amplitudes(numOctaves, 1.0);
    return PerlinNoise(random, octaveStart, amplitudes, false);
}

PerlinNoise::PerlinNoise(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes, bool useNewInitialization)
    : m_firstOctave(firstOctave)
    , m_amplitudes(amplitudes)
{
    // Reference: PerlinNoise.java lines 84-133

    int32_t octaves = static_cast<int32_t>(m_amplitudes.size());
    int32_t zeroOctaveIndex = -m_firstOctave;
    m_noiseLevels.resize(octaves, nullptr);

    if (useNewInitialization) {
        // Modern initialization (post-1.18)
        // Reference: PerlinNoise.java lines 90-98
        XoroshiroPositionalRandomFactory positional = random.forkPositional();

        for (int32_t i = 0; i < octaves; ++i) {
            if (m_amplitudes[i] != static_cast<double>(0.0F)) {
                int32_t octave = m_firstOctave + i;

                // Create string "octave_N"
                std::stringstream ss;
                ss << "octave_" << octave;

                XoroshiroRandomSource octaveRandom = positional.fromHashOf(ss.str());
                m_noiseLevels[i] = new ImprovedNoise(octaveRandom);
            }
        }
    } else {
        // Legacy initialization (pre-1.18)
        // Reference: PerlinNoise.java lines 99-128
        ImprovedNoise* zeroOctave = new ImprovedNoise(random);

        if (zeroOctaveIndex >= 0 && zeroOctaveIndex < octaves) {
            double zeroOctaveAmplitude = m_amplitudes[zeroOctaveIndex];
            if (zeroOctaveAmplitude != static_cast<double>(0.0F)) {
                m_noiseLevels[zeroOctaveIndex] = zeroOctave;
            } else {
                delete zeroOctave;
                zeroOctave = nullptr;
            }
        } else {
            delete zeroOctave;
            zeroOctave = nullptr;
        }

        for (int32_t i = zeroOctaveIndex - 1; i >= 0; --i) {
            if (i < octaves) {
                double amplitude = m_amplitudes[i];
                if (amplitude != static_cast<double>(0.0F)) {
                    m_noiseLevels[i] = new ImprovedNoise(random);
                } else {
                    skipOctave(random);
                }
            } else {
                skipOctave(random);
            }
        }

        // Verify correct number of noise levels created
        int32_t nonNullCount = 0;
        int32_t nonZeroAmpCount = 0;
        for (size_t i = 0; i < m_noiseLevels.size(); ++i) {
            if (m_noiseLevels[i] != nullptr) nonNullCount++;
            if (m_amplitudes[i] != static_cast<double>(0.0F)) nonZeroAmpCount++;
        }
        if (nonNullCount != nonZeroAmpCount) {
            throw std::runtime_error("Failed to create correct number of noise levels for given non-zero amplitudes");
        }

        if (zeroOctaveIndex < octaves - 1) {
            throw std::invalid_argument("Positive octaves are temporarily disabled");
        }
    }

    // Calculate scaling factors
    // Reference: PerlinNoise.java lines 130-132
    m_lowestFreqInputFactor = std::pow(static_cast<double>(2.0F), static_cast<double>(-zeroOctaveIndex));
    m_lowestFreqValueFactor = std::pow(static_cast<double>(2.0F), static_cast<double>(octaves - 1)) /
                              (std::pow(static_cast<double>(2.0F), static_cast<double>(octaves)) - static_cast<double>(1.0F));
    m_maxValue = edgeValue(static_cast<double>(2.0F));
}

PerlinNoise::PerlinNoise(LegacyRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes, bool useNewInitialization)
    : m_firstOctave(firstOctave)
    , m_amplitudes(amplitudes)
{
    int32_t octaves = static_cast<int32_t>(m_amplitudes.size());
    int32_t zeroOctaveIndex = -m_firstOctave;
    m_noiseLevels.resize(octaves, nullptr);

    if (useNewInitialization) {
        LegacyPositionalRandomFactory positional = random.forkPositional();

        for (int32_t i = 0; i < octaves; ++i) {
            if (m_amplitudes[i] != static_cast<double>(0.0F)) {
                int32_t octave = m_firstOctave + i;
                std::stringstream ss;
                ss << "octave_" << octave;

                LegacyRandomSource octaveRandom = positional.fromHashOf(ss.str());
                m_noiseLevels[i] = new ImprovedNoise(octaveRandom);
            }
        }
    } else {
        ImprovedNoise* zeroOctave = new ImprovedNoise(random);

        if (zeroOctaveIndex >= 0 && zeroOctaveIndex < octaves) {
            double zeroOctaveAmplitude = m_amplitudes[zeroOctaveIndex];
            if (zeroOctaveAmplitude != static_cast<double>(0.0F)) {
                m_noiseLevels[zeroOctaveIndex] = zeroOctave;
            } else {
                delete zeroOctave;
                zeroOctave = nullptr;
            }
        } else {
            delete zeroOctave;
            zeroOctave = nullptr;
        }

        for (int32_t i = zeroOctaveIndex - 1; i >= 0; --i) {
            if (i < octaves) {
                double amplitude = m_amplitudes[i];
                if (amplitude != static_cast<double>(0.0F)) {
                    m_noiseLevels[i] = new ImprovedNoise(random);
                } else {
                    skipOctave(random);
                }
            } else {
                skipOctave(random);
            }
        }

        int32_t nonNullCount = 0;
        int32_t nonZeroAmpCount = 0;
        for (size_t i = 0; i < m_noiseLevels.size(); ++i) {
            if (m_noiseLevels[i] != nullptr) nonNullCount++;
            if (m_amplitudes[i] != static_cast<double>(0.0F)) nonZeroAmpCount++;
        }
        if (nonNullCount != nonZeroAmpCount) {
            throw std::runtime_error("Failed to create correct number of noise levels for given non-zero amplitudes");
        }

        if (zeroOctaveIndex < octaves - 1) {
            throw std::invalid_argument("Positive octaves are temporarily disabled");
        }
    }

    m_lowestFreqInputFactor = std::pow(static_cast<double>(2.0F), static_cast<double>(-zeroOctaveIndex));
    m_lowestFreqValueFactor = std::pow(static_cast<double>(2.0F), static_cast<double>(octaves - 1)) /
                              (std::pow(static_cast<double>(2.0F), static_cast<double>(octaves)) - static_cast<double>(1.0F));
    m_maxValue = edgeValue(static_cast<double>(2.0F));
}

PerlinNoise::PerlinNoise(levelgen::WorldgenRandom& random, int32_t firstOctave, const std::vector<double>& amplitudes, bool useNewInitialization)
    : m_firstOctave(firstOctave)
    , m_amplitudes(amplitudes)
{
    int32_t octaves = static_cast<int32_t>(m_amplitudes.size());
    int32_t zeroOctaveIndex = -m_firstOctave;
    m_noiseLevels.resize(octaves, nullptr);

    if (useNewInitialization) {
        if (random.usesLegacyRandomSource()) {
            LegacyPositionalRandomFactory positional = random.forkLegacyPositional();

            for (int32_t i = 0; i < octaves; ++i) {
                if (m_amplitudes[i] != static_cast<double>(0.0F)) {
                    int32_t octave = m_firstOctave + i;
                    std::stringstream ss;
                    ss << "octave_" << octave;

                    levelgen::WorldgenRandom octaveRandom{positional.fromHashOf(ss.str())};
                    m_noiseLevels[i] = new ImprovedNoise(octaveRandom);
                }
            }
        } else {
            XoroshiroPositionalRandomFactory positional = random.forkPositional();

            for (int32_t i = 0; i < octaves; ++i) {
                if (m_amplitudes[i] != static_cast<double>(0.0F)) {
                    int32_t octave = m_firstOctave + i;
                    std::stringstream ss;
                    ss << "octave_" << octave;

                    levelgen::WorldgenRandom octaveRandom{positional.fromHashOf(ss.str())};
                    m_noiseLevels[i] = new ImprovedNoise(octaveRandom);
                }
            }
        }
    } else {
        ImprovedNoise* zeroOctave = new ImprovedNoise(random);

        if (zeroOctaveIndex >= 0 && zeroOctaveIndex < octaves) {
            double zeroOctaveAmplitude = m_amplitudes[zeroOctaveIndex];
            if (zeroOctaveAmplitude != static_cast<double>(0.0F)) {
                m_noiseLevels[zeroOctaveIndex] = zeroOctave;
            } else {
                delete zeroOctave;
                zeroOctave = nullptr;
            }
        } else {
            delete zeroOctave;
            zeroOctave = nullptr;
        }

        for (int32_t i = zeroOctaveIndex - 1; i >= 0; --i) {
            if (i < octaves) {
                double amplitude = m_amplitudes[i];
                if (amplitude != static_cast<double>(0.0F)) {
                    m_noiseLevels[i] = new ImprovedNoise(random);
                } else {
                    skipOctave(random);
                }
            } else {
                skipOctave(random);
            }
        }

        int32_t nonNullCount = 0;
        int32_t nonZeroAmpCount = 0;
        for (size_t i = 0; i < m_noiseLevels.size(); ++i) {
            if (m_noiseLevels[i] != nullptr) nonNullCount++;
            if (m_amplitudes[i] != static_cast<double>(0.0F)) nonZeroAmpCount++;
        }
        if (nonNullCount != nonZeroAmpCount) {
            throw std::runtime_error("Failed to create correct number of noise levels for given non-zero amplitudes");
        }

        if (zeroOctaveIndex < octaves - 1) {
            throw std::invalid_argument("Positive octaves are temporarily disabled");
        }
    }

    m_lowestFreqInputFactor = std::pow(static_cast<double>(2.0F), static_cast<double>(-zeroOctaveIndex));
    m_lowestFreqValueFactor = std::pow(static_cast<double>(2.0F), static_cast<double>(octaves - 1)) /
                              (std::pow(static_cast<double>(2.0F), static_cast<double>(octaves)) - static_cast<double>(1.0F));
    m_maxValue = edgeValue(static_cast<double>(2.0F));
}

double PerlinNoise::getValue(double x, double y, double z) const {
    // Reference: PerlinNoise.java lines 143-145
    return getValue(x, y, z, static_cast<double>(0.0F), static_cast<double>(0.0F), false);
}

double PerlinNoise::getValue(double x, double y, double z, double yScale, double yFudge, bool yFlatHack) const {
    // Reference: PerlinNoise.java lines 149-166

    double value = static_cast<double>(0.0F);
    double factor = m_lowestFreqInputFactor;
    double valueFactor = m_lowestFreqValueFactor;

    for (size_t i = 0; i < m_noiseLevels.size(); ++i) {
        ImprovedNoise* noise = m_noiseLevels[i];
        if (noise != nullptr) {
            // CRITICAL: yFlatHack uses -noise->yo instead of wrapping y coordinate
            double noiseVal = noise->noise(
                wrap(x * factor),
                yFlatHack ? -noise->getYo() : wrap(y * factor),
                wrap(z * factor),
                yScale * factor,
                yFudge * factor
            );
            value += m_amplitudes[i] * noiseVal * valueFactor;
        }

        factor *= static_cast<double>(2.0F);
        valueFactor /= static_cast<double>(2.0F);
    }

    return value;
}

double PerlinNoise::maxBrokenValue(double yScale) const {
    // Reference: PerlinNoise.java lines 168-170
    return edgeValue(yScale + static_cast<double>(2.0F));
}

double PerlinNoise::edgeValue(double noiseValue) const {
    // Reference: PerlinNoise.java lines 172-186

    double value = static_cast<double>(0.0F);
    double valueFactor = m_lowestFreqValueFactor;

    for (size_t i = 0; i < m_noiseLevels.size(); ++i) {
        ImprovedNoise* noise = m_noiseLevels[i];
        if (noise != nullptr) {
            value += m_amplitudes[i] * noiseValue * valueFactor;
        }

        valueFactor /= static_cast<double>(2.0F);
    }

    return value;
}

ImprovedNoise* PerlinNoise::getOctaveNoise(int32_t i) const {
    // Reference: PerlinNoise.java lines 188-190
    return m_noiseLevels[m_noiseLevels.size() - 1 - i];
}

double PerlinNoise::wrap(double x) {
    // Reference: PerlinNoise.java lines 192-194
    // Wraps coordinates to range to prevent floating-point precision issues
    // Formula: x - lfloor(x / ROUND_OFF + 0.5) * ROUND_OFF
    return x - static_cast<double>(Mth::lfloor(x / static_cast<double>(3.3554432E7F) + static_cast<double>(0.5F))) * static_cast<double>(3.3554432E7F);
}

void PerlinNoise::skipOctave(XoroshiroRandomSource& random) {
    // Reference: PerlinNoise.java lines 139-141
    // Skip 262 random values to match Java's ImprovedNoise constructor consumption
    // ImprovedNoise constructor calls:
    //   - nextDouble() 3 times (for xo, yo, zo)
    //   - nextInt(256-i) 256 times (for permutation table shuffle)
    // The 256 nextInt calls each consume nextLong(), so 3 + 256 = 259
    // But Java's consumeCount(262) must account for something else...
    // Let me use the exact number from Java
    for (int i = 0; i < 262; ++i) {
        random.nextLong();
    }
}

void PerlinNoise::skipOctave(LegacyRandomSource& random) {
    random.consumeCount(262);
}

void PerlinNoise::skipOctave(levelgen::WorldgenRandom& random) {
    random.consumeCount(262);
}

} // namespace minecraft
