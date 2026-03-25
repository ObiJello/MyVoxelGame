#pragma once

#include "levelgen/WorldgenRandom.h"
#include "synth/SimplexNoise.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include <vector>
#include <memory>
#include <cmath>
#include <set>

// Reference: net/minecraft/world/level/levelgen/synth/PerlinSimplexNoise.java

namespace minecraft {
namespace synth {

/**
 * Multi-octave Perlin Simplex Noise
 * Reference: PerlinSimplexNoise.java
 *
 * Combines multiple SimplexNoise instances at different frequencies
 * to create more natural-looking noise patterns.
 */
class PerlinSimplexNoise {
private:
    std::vector<std::unique_ptr<SimplexNoise>> m_noiseLevels;
    double m_highestFreqValueFactor;
    double m_highestFreqInputFactor;

    template <typename RandomT>
    void initialize(RandomT& random, const std::vector<int>& octaveSet) {
        if (octaveSet.empty()) {
            throw std::invalid_argument("Need some octaves!");
        }

        std::set<int> octaves(octaveSet.begin(), octaveSet.end());

        int lowFreqOctaves = -*octaves.begin();
        int highFreqOctaves = *octaves.rbegin();
        int totalOctaves = lowFreqOctaves + highFreqOctaves + 1;

        if (totalOctaves < 1) {
            throw std::invalid_argument("Total number of octaves needs to be >= 1");
        }

        m_noiseLevels.resize(totalOctaves);

        auto zeroOctave = std::make_unique<SimplexNoise>(random);
        int zeroOctaveIndex = highFreqOctaves;
        SimplexNoise* zeroOctavePtr = zeroOctave.get();
        double zeroXo = zeroOctavePtr->getXo();
        double zeroYo = zeroOctavePtr->getYo();
        double zeroZo = zeroOctavePtr->getZo();

        if (highFreqOctaves >= 0 && highFreqOctaves < totalOctaves && octaves.count(0)) {
            m_noiseLevels[highFreqOctaves] = std::move(zeroOctave);
        }

        for (int i = highFreqOctaves + 1; i < totalOctaves; ++i) {
            if (i >= 0 && octaves.count(zeroOctaveIndex - i)) {
                m_noiseLevels[i] = std::make_unique<SimplexNoise>(random);
            } else {
                random.consumeCount(262);
            }
        }

        if (highFreqOctaves > 0) {
            double seedValue = 0.0;
            if (m_noiseLevels[highFreqOctaves]) {
                seedValue = m_noiseLevels[highFreqOctaves]->getValue(zeroXo, zeroYo, zeroZo);
            } else {
                SimplexNoise tempNoise(random);
                seedValue = tempNoise.getValue(zeroXo, zeroYo, zeroZo);
            }

            int64_t positiveOctaveSeed = static_cast<int64_t>(seedValue * static_cast<double>(INT64_MAX));
            levelgen::WorldgenRandom highFreqRandom{LegacyRandomSource(positiveOctaveSeed)};

            for (int i = zeroOctaveIndex - 1; i >= 0; --i) {
                if (i < totalOctaves && octaves.count(zeroOctaveIndex - i)) {
                    m_noiseLevels[i] = std::make_unique<SimplexNoise>(highFreqRandom);
                } else {
                    highFreqRandom.consumeCount(262);
                }
            }
        }

        m_highestFreqInputFactor = std::pow(2.0, static_cast<double>(highFreqOctaves));
        m_highestFreqValueFactor = 1.0 / (std::pow(2.0, static_cast<double>(totalOctaves)) - 1.0);
    }

public:
    /**
     * Construct with random source and list of octaves
     * Reference: PerlinSimplexNoise.java lines 16-62
     *
     * @param random Random source for noise initialization
     * @param octaveSet List of octave indices (e.g., {0} for single octave, {-1, 0, 1} for three octaves)
     */
    PerlinSimplexNoise(LegacyRandomSource& random, const std::vector<int>& octaveSet) {
        initialize(random, octaveSet);
    }

    PerlinSimplexNoise(levelgen::WorldgenRandom& random, const std::vector<int>& octaveSet) {
        if (random.usesLegacyRandomSource()) {
            initialize(random.getLegacyRandomSource(), octaveSet);
        } else {
            initialize(random.getRandomSource(), octaveSet);
        }
    }

    /**
     * Get 2D noise value
     * Reference: PerlinSimplexNoise.java lines 64-79
     *
     * @param x X coordinate
     * @param y Y coordinate (actually Z in world coords)
     * @param useNoiseStart Whether to add noise offsets
     * @return Noise value in range approximately [-1, 1]
     */
    double getValue(double x, double y, bool useNoiseStart = false) const {
        double value = 0.0;
        double factor = m_highestFreqInputFactor;
        double valueFactor = m_highestFreqValueFactor;

        for (const auto& noiseLevel : m_noiseLevels) {
            if (noiseLevel) {
                double offsetX = useNoiseStart ? noiseLevel->getXo() : 0.0;
                double offsetY = useNoiseStart ? noiseLevel->getYo() : 0.0;
                value += noiseLevel->getValue(x * factor + offsetX, y * factor + offsetY) * valueFactor;
            }
            factor /= 2.0;
            valueFactor *= 2.0;
        }

        return value;
    }
};

/**
 * BiomeInfoNoise - Static noise used for biome-based feature counts
 * Reference: Biome.java line 47, 236
 *
 * This is Biome.BIOME_INFO_NOISE, created with seed 2345L and octave [0]
 */
class BiomeInfoNoise {
private:
    static std::unique_ptr<PerlinSimplexNoise> s_instance;
    static bool s_initialized;

    static void ensureInitialized() {
        if (!s_initialized) {
            levelgen::WorldgenRandom random{LegacyRandomSource(2345L)};
            s_instance = std::make_unique<PerlinSimplexNoise>(random, std::vector<int>{0});
            s_initialized = true;
        }
    }

public:
    /**
     * Get noise value at position
     * Reference: Used by NoiseBasedCountPlacement and NoiseThresholdCountPlacement
     *
     * @param x X coordinate (typically blockX / factor)
     * @param z Z coordinate (typically blockZ / factor)
     * @return Noise value
     */
    static double getValue(double x, double z) {
        ensureInitialized();
        return s_instance->getValue(x, z, false);
    }
};

} // namespace synth
} // namespace minecraft
