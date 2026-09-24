#pragma once

#include "external/json.hpp"
#include "levelgen/density/Interval.h"
#include "levelgen/density/synth/Noise.h"
#include "random/AnyPositionalRandomFactory.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Reference: synth.NormalNoise (26.3) - the worldgen/noise registry entry
// ("minecraft:ridge", ...). 26.3 turned it into a noise DEFINITION: the
// parameters build a list of octaves once, and create(random) instantiates the
// octaves as a NoiseStack of two Perlin octaves each (the second at
// INPUT_FACTOR times the frequency), seeded per octave from
// fromHashOf("octave_<index>"). The normalization factor scales the stack to a
// target deviation of 1/3 per unit amplitude.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class NormalNoise;
using NormalNoisePtr = std::shared_ptr<const NormalNoise>;

class NormalNoise final {
public:
    static constexpr double TARGET_DEVIATION = 0.3333333333333333;

    // NormalNoise.Normalization.
    enum class Normalization { DISABLED, ENABLED, LEGACY };

    // NormalNoise.Parameters (record).
    struct Parameters {
        double baseAmplitude;
        int baseOctave;
        int octaveCount;
        Normalization normalize;
        std::vector<double> amplitudeModifiers;

        bool operator==(const Parameters& other) const;
        bool operator!=(const Parameters& other) const { return !(*this == other); }
        size_t hashCode() const;
    };

    class Builder {
    public:
        Builder& setBaseAmplitude(double baseAmplitude);
        Builder& setBaseOctave(int baseOctave);
        Builder& setOctaveCount(int octaveCount);
        Builder& setNormalize(bool normalize);
        Builder& setLegacyNormalization();   // deprecated in Java
        Builder& setAmplitudeModifier(int index, double value);
        NormalNoisePtr build() const;

    private:
        friend class NormalNoise;
        Builder() = default;

        double m_baseAmplitude = 1.0;
        int m_baseOctave = 0;
        int m_octaveCount = 1;
        Normalization m_normalize = Normalization::ENABLED;
        std::vector<double> m_amplitudeModifiers;
    };

    static Builder builder() { return Builder(); }

    // Throws std::invalid_argument for an empty amplitude list.
    static NormalNoisePtr createParity(int firstOctave, const std::vector<double>& amplitudes);

    // NormalNoise.DIRECT_CODEC. Throws std::runtime_error with the codec's
    // message when the JSON does not decode.
    static NormalNoisePtr fromJson(const nlohmann::json& json);

    NoisePtr create(random::AnyRandomSource& random) const;
    // Deprecated in Java: the legacy Nether biome noise (temperature/vegetation).
    NoisePtr createForLegacyNetherBiome(random::AnyRandomSource& random) const;

    Interval range() const { return m_range; }
    const Parameters& parameters() const { return m_parameters; }

    // Java equals/hashCode: parameter equality.
    bool operator==(const NormalNoise& other) const { return this == &other || m_parameters == other.m_parameters; }
    bool operator!=(const NormalNoise& other) const { return !(*this == other); }
    size_t hashCode() const { return m_parameters.hashCode(); }

    // NormalNoise.OctaveInfo (record).
    struct OctaveInfo {
        int octaveIndex;
        double frequency;
        double amplitude;

        std::string seed() const { return "octave_" + std::to_string(octaveIndex); }
        double absAmplitude() const;
    };

private:
    explicit NormalNoise(Parameters parameters);

    static std::vector<OctaveInfo> buildOctaves(int baseOctave, double baseAmplitude, int octaveCount,
                                                bool normalize, const std::vector<double>& amplitudeModifiers);
    static double getAmplitudeModifier(const std::vector<double>& amplitudeModifiers, int index);
    static double computeNormalizationFactor(double targetAmplitude, const std::vector<OctaveInfo>& octaves);
    static double estimateDeviation(const std::vector<OctaveInfo>& octaves);
    static double computeParityBaseAmplitude(int baseOctave, const std::vector<double>& amplitudes);
    static double computeParityNormalizationFactor(double baseAmplitude, int octaveCount,
                                                   const std::vector<double>& amplitudeModifiers);
    static double parityExpectedDeviation(int octaveSpan);

    Parameters m_parameters;
    std::vector<OctaveInfo> m_octaves;
    double m_normalizationFactor;
    Interval m_range;
};

// Reference: levelgen.Noises.instantiate (26.3) - a registry noise's sampler:
// noise.create(random.fromHashOf(key)), key being the full identifier
// ("minecraft:ridge"). Defined in Noises.cpp.
NoisePtr instantiate(const NormalNoise& noise, const std::string& key,
                     const random::AnyPositionalRandomFactory& random);

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
