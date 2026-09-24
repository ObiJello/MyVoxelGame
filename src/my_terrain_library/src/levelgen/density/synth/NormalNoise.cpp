#include "levelgen/density/synth/NormalNoise.h"

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/synth/LegacyFbmInitializer.h"
#include "levelgen/density/synth/NoiseStack.h"
#include "levelgen/density/synth/PerlinNoise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>

// Reference: synth.NormalNoise (26.3), plus Holder<NormalNoise> equality
// (levelgen/density/DensityFunction.h NoiseHolder).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

namespace {

constexpr double INPUT_FACTOR = 1.0181268882175227;
constexpr int MAX_OCTAVE_COUNT = 32;
constexpr double MAX_AMPLITUDE = 1000000.0;

// DoubleStream.sum() (JDK 18+ Collectors.sumWithCompensation /
// computeFinalSum): Kahan-compensated, NOT a plain left-to-right sum.
double doubleStreamSum(const std::vector<NormalNoise::OctaveInfo>& octaves) {
    double sum = 0.0;          // intermediateSum[0]
    double compensation = 0.0; // intermediateSum[1]
    double simpleSum = 0.0;    // intermediateSum[2]
    for (const NormalNoise::OctaveInfo& octave : octaves) {
        const double value = octave.absAmplitude();
        const double tmp = value - compensation;
        const double velvel = sum + tmp;
        compensation = (velvel - sum) - tmp;
        sum = velvel;
        simpleSum += value;
    }
    const double tmp = sum - compensation;
    if (std::isnan(tmp) && std::isinf(simpleSum)) {
        return simpleSum;
    }
    return tmp;
}

} // namespace

// ---- Parameters / OctaveInfo ---------------------------------------------------

bool NormalNoise::Parameters::operator==(const Parameters& other) const {
    // Record equality: Double.compare for baseAmplitude; DoubleList.equals
    // compares elements with == (dcmpl).
    if (!doubleEq(baseAmplitude, other.baseAmplitude) || baseOctave != other.baseOctave ||
        octaveCount != other.octaveCount || normalize != other.normalize ||
        amplitudeModifiers.size() != other.amplitudeModifiers.size()) {
        return false;
    }
    for (size_t i = 0; i < amplitudeModifiers.size(); ++i) {
        if (amplitudeModifiers[i] != other.amplitudeModifiers[i]) {
            return false;
        }
    }
    return true;
}

size_t NormalNoise::Parameters::hashCode() const {
    size_t h = hashDouble(baseAmplitude);
    h = hashCombine(h, std::hash<int>()(baseOctave));
    h = hashCombine(h, std::hash<int>()(octaveCount));
    h = hashCombine(h, std::hash<int>()(static_cast<int>(normalize)));
    for (double modifier : amplitudeModifiers) {
        // == equality: -0.0 and +0.0 must hash alike.
        h = hashCombine(h, hashDouble(modifier == 0.0 ? 0.0 : modifier));
    }
    return h;
}

double NormalNoise::OctaveInfo::absAmplitude() const {
    return std::fabs(amplitude);
}

// ---- NormalNoise ---------------------------------------------------------------

NormalNoise::NormalNoise(Parameters parameters)
    : m_parameters(std::move(parameters)),
      m_octaves(buildOctaves(m_parameters.baseOctave, m_parameters.baseAmplitude, m_parameters.octaveCount,
                             m_parameters.normalize != Normalization::DISABLED, m_parameters.amplitudeModifiers)),
      m_normalizationFactor(0.0),
      m_range(Interval::ofExact(0.0f)) {
    double targetAmplitude = doubleStreamSum(m_octaves);
    double normalizationFactor = computeNormalizationFactor(targetAmplitude, m_octaves);
    if (m_parameters.normalize == Normalization::LEGACY && normalizationFactor != 0.0) {
        const double parityNormalizationFactor = computeParityNormalizationFactor(
            m_parameters.baseAmplitude, m_parameters.octaveCount, m_parameters.amplitudeModifiers);
        targetAmplitude *= parityNormalizationFactor / normalizationFactor;
        normalizationFactor = parityNormalizationFactor;
    }

    m_normalizationFactor = normalizationFactor;
    m_range = Interval::ofSymmetric(static_cast<float>(targetAmplitude * 0.3333333333333333 * 6.0));
}

std::vector<NormalNoise::OctaveInfo> NormalNoise::buildOctaves(int baseOctave, double baseAmplitude, int octaveCount,
                                                               bool normalize,
                                                               const std::vector<double>& amplitudeModifiers) {
    double frequency = std::pow(2.0, static_cast<double>(baseOctave));
    double amplitude = baseAmplitude;
    if (normalize) {
        amplitude *= std::pow(0.5, static_cast<double>(-(octaveCount - 1))) /
                     (std::pow(0.5, static_cast<double>(-octaveCount)) - 1.0);
    }

    std::vector<OctaveInfo> octaves;
    octaves.reserve(static_cast<size_t>(std::max(octaveCount, 0)));
    for (int i = 0; i < octaveCount; ++i) {
        const double amplitudeModifier = getAmplitudeModifier(amplitudeModifiers, i);
        if (amplitudeModifier != 0.0) {
            const double modifiedAmplitude = amplitude * amplitudeModifier;
            octaves.push_back(OctaveInfo{baseOctave + i, frequency, modifiedAmplitude});
        }
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return octaves;
}

double NormalNoise::getAmplitudeModifier(const std::vector<double>& amplitudeModifiers, int index) {
    return amplitudeModifiers.empty() ? 1.0 : amplitudeModifiers.at(static_cast<size_t>(index));
}

double NormalNoise::computeNormalizationFactor(double targetAmplitude, const std::vector<OctaveInfo>& octaves) {
    const double inputDeviation = estimateDeviation(octaves);
    if (inputDeviation == 0.0) {
        return 0.0;
    }
    const double inputSumDeviation = inputDeviation * std::sqrt(2.0);
    const double targetDeviation = targetAmplitude * 0.3333333333333333;
    return targetDeviation / inputSumDeviation;
}

double NormalNoise::estimateDeviation(const std::vector<OctaveInfo>& octaves) {
    double variance = 0.0;
    for (const OctaveInfo& octave : octaves) {
        const double layerDeviation = 0.2702247831245211 * octave.absAmplitude();
        variance += layerDeviation * layerDeviation; // Mth.square(double)
    }
    return std::sqrt(variance);
}

NormalNoisePtr NormalNoise::createParity(int firstOctave, const std::vector<double>& amplitudes) {
    if (amplitudes.empty()) {
        throw std::invalid_argument("Need at least 1 amplitude");
    }
    Builder parameters = builder();
    parameters.setBaseOctave(firstOctave);
    parameters.setOctaveCount(static_cast<int>(amplitudes.size()));
    parameters.setBaseAmplitude(computeParityBaseAmplitude(parameters.m_baseOctave, amplitudes));

    for (size_t i = 0; i < amplitudes.size(); ++i) {
        const double modifier = amplitudes[i];
        if (modifier != 1.0) {
            parameters.setAmplitudeModifier(static_cast<int>(i), modifier);
        }
    }
    return parameters.build();
}

double NormalNoise::computeParityBaseAmplitude(int baseOctave, const std::vector<double>& amplitudes) {
    const std::vector<OctaveInfo> octaves =
        buildOctaves(baseOctave, 1.0, static_cast<int>(amplitudes.size()), true, amplitudes);
    const double targetAmplitude = doubleStreamSum(octaves);
    const double newNormalizationFactor = computeNormalizationFactor(targetAmplitude, octaves);
    if (newNormalizationFactor == 0.0) {
        return 1.0;
    }
    const double oldNormalizationFactor =
        computeParityNormalizationFactor(1.0, static_cast<int>(amplitudes.size()), amplitudes);
    return oldNormalizationFactor / newNormalizationFactor;
}

double NormalNoise::computeParityNormalizationFactor(double baseAmplitude, int octaveCount,
                                                     const std::vector<double>& amplitudeModifiers) {
    int minOctave = std::numeric_limits<int32_t>::max();
    int maxOctave = std::numeric_limits<int32_t>::min();
    for (int i = 0; i < octaveCount; ++i) {
        const double modifier = getAmplitudeModifier(amplitudeModifiers, i);
        if (modifier != 0.0) {
            minOctave = std::min(minOctave, i);
            maxOctave = std::max(maxOctave, i);
        }
    }
    // Java int subtraction wraps (the library builds with -fwrapv).
    return baseAmplitude * 0.5 * 0.3333333333333333 / parityExpectedDeviation(maxOctave - minOctave);
}

double NormalNoise::parityExpectedDeviation(int octaveSpan) {
    return 0.1 * (1.0 + 1.0 / static_cast<double>(octaveSpan + 1));
}

NoisePtr NormalNoise::create(random::AnyRandomSource& random) const {
    const random::AnyPositionalRandomFactory firstRandom = random.forkPositional();
    const random::AnyPositionalRandomFactory secondRandom = random.forkPositional();
    NoiseStack::Builder stack = NoiseStack::builder();

    for (const OctaveInfo& octave : m_octaves) {
        const std::string octaveSeed = octave.seed();
        random::AnyRandomSource firstSource = firstRandom.fromHashOf(octaveSeed);
        NoisePtr firstNoise = std::make_shared<const PerlinNoise>(firstSource);
        random::AnyRandomSource secondSource = secondRandom.fromHashOf(octaveSeed);
        NoisePtr secondNoise = std::make_shared<const PerlinNoise>(secondSource);
        const float valueFactor = static_cast<float>(m_normalizationFactor * octave.amplitude);
        stack.add(firstNoise, octave.frequency, valueFactor);
        stack.add(secondNoise, octave.frequency * INPUT_FACTOR, valueFactor);
    }
    return stack.build();
}

NoisePtr NormalNoise::createForLegacyNetherBiome(random::AnyRandomSource& random) const {
    std::vector<double> amplitudes = m_parameters.amplitudeModifiers;
    if (amplitudes.empty()) {
        amplitudes.assign(static_cast<size_t>(m_parameters.octaveCount), 1.0);
    }

    const NoiseStackPtr first =
        LegacyFbmInitializer::createForLegacyNetherBiome(random, m_parameters.baseOctave, amplitudes);
    const NoiseStackPtr second =
        LegacyFbmInitializer::createForLegacyNetherBiome(random, m_parameters.baseOctave, amplitudes);
    const float valueFactor = static_cast<float>(m_normalizationFactor * m_parameters.baseAmplitude);
    NoiseStack::Builder stack = NoiseStack::builder();
    stack.addStack(*first, 1.0, valueFactor);
    stack.addStack(*second, INPUT_FACTOR, valueFactor);
    return stack.build();
}

// ---- Builder -------------------------------------------------------------------

NormalNoise::Builder& NormalNoise::Builder::setBaseAmplitude(double baseAmplitude) {
    m_baseAmplitude = baseAmplitude;
    return *this;
}

NormalNoise::Builder& NormalNoise::Builder::setBaseOctave(int baseOctave) {
    m_baseOctave = baseOctave;
    return *this;
}

NormalNoise::Builder& NormalNoise::Builder::setOctaveCount(int octaveCount) {
    if (!m_amplitudeModifiers.empty()) {
        throw std::invalid_argument("Cannot set octave count after setting amplitude modifier");
    }
    m_octaveCount = octaveCount;
    return *this;
}

NormalNoise::Builder& NormalNoise::Builder::setNormalize(bool normalize) {
    m_normalize = normalize ? Normalization::ENABLED : Normalization::DISABLED;
    return *this;
}

NormalNoise::Builder& NormalNoise::Builder::setLegacyNormalization() {
    m_normalize = Normalization::LEGACY;
    return *this;
}

NormalNoise::Builder& NormalNoise::Builder::setAmplitudeModifier(int index, double value) {
    if (index >= 0 && index < m_octaveCount) {
        if (m_amplitudeModifiers.empty()) {
            m_amplitudeModifiers.assign(static_cast<size_t>(m_octaveCount), 1.0);
        }
        m_amplitudeModifiers[static_cast<size_t>(index)] = value;
        return *this;
    }
    throw std::invalid_argument(std::to_string(index) + " outside of octave range [0; " +
                                std::to_string(m_octaveCount) + ")");
}

NormalNoisePtr NormalNoise::Builder::build() const {
    Parameters parameters{m_baseAmplitude, m_baseOctave, m_octaveCount, m_normalize, m_amplitudeModifiers};
    return NormalNoisePtr(new NormalNoise(std::move(parameters)));
}

// ---- DIRECT_CODEC ----------------------------------------------------------------
//
// Parameters.CODEC (RecordCodecBuilder) read through JsonOps (checked against
// the 26.3 jar's DFU):
//   base_amplitude       optional, default 1.0, doubleRange(9.999999747378752E-6, 1000000.0)
//   base_octave          required, intRange(-32, 32)
//   octave_count         optional, default 1, intRange(1, 32)
//   normalize            optional, default ENABLED; Either<BOOL, "legacy">
//   amplitude_modifiers  optional, default [], doubleRange(0.0, 1000000.0).listOf(0, 32)
// then Parameters::validate (non-empty modifiers must have octave_count
// entries). optionalFieldOf is strict: a present but invalid field is an
// error, not the default. Unknown fields are ignored; a JSON null reads as
// absent (JsonOps' MapLike.get).

namespace {

[[noreturn]] void codecError(const std::string& message) {
    throw std::runtime_error("NormalNoise: " + message);
}

std::string javaDoubleString(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

// Double.compare / Double.compareTo (-0.0 < +0.0, NaN greatest).
int javaDoubleCompare(double a, double b) {
    if (a < b) return -1;
    if (a > b) return 1;
    const bool aNaN = a != a;
    const bool bNaN = b != b;
    if (aNaN || bNaN) return aNaN == bNaN ? 0 : (aNaN ? 1 : -1);
    const bool aNeg = std::signbit(a);
    const bool bNeg = std::signbit(b);
    return aNeg == bNeg ? 0 : (aNeg ? -1 : 1);
}

const nlohmann::json* field(const nlohmann::json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end() || it->is_null()) {
        return nullptr;
    }
    return &*it;
}

// JsonOps.getNumberValue(...).doubleValue(): JSON numbers only (a boolean or
// a numeric string is "Not a number").
double readDouble(const nlohmann::json& value, const char* name) {
    if (value.is_number_integer() && !value.is_number_unsigned()) return static_cast<double>(value.get<int64_t>());
    if (value.is_number_unsigned()) return static_cast<double>(value.get<uint64_t>());
    if (value.is_number_float()) return value.get<double>();
    codecError(std::string("Not a number: ") + value.dump() + " (" + name + ")");
}

// JsonOps.getNumberValue(...).intValue() (Gson LazilyParsedNumber: an
// integer keeps its low 32 bits, a fraction truncates towards zero first).
int32_t readInt(const nlohmann::json& value, const char* name) {
    if (value.is_number_unsigned()) return static_cast<int32_t>(static_cast<uint32_t>(value.get<uint64_t>()));
    if (value.is_number_integer()) return static_cast<int32_t>(static_cast<uint32_t>(value.get<int64_t>()));
    if (value.is_number_float()) {
        const double truncated = std::trunc(value.get<double>());
        if (!std::isfinite(truncated)) codecError(std::string("Not a number: ") + value.dump() + " (" + name + ")");
        double low = std::fmod(truncated, 4294967296.0);
        if (low < 0.0) low += 4294967296.0;
        return static_cast<int32_t>(static_cast<uint32_t>(low));
    }
    codecError(std::string("Not a number: ") + value.dump() + " (" + name + ")");
}

double checkDoubleRange(double value, double minInclusive, double maxInclusive) {
    if (javaDoubleCompare(value, minInclusive) >= 0 && javaDoubleCompare(value, maxInclusive) <= 0) {
        return value;
    }
    codecError("Value " + javaDoubleString(value) + " outside of range [" + javaDoubleString(minInclusive) + ":" +
               javaDoubleString(maxInclusive) + "]");
}

int checkIntRange(int value, int minInclusive, int maxInclusive) {
    if (value >= minInclusive && value <= maxInclusive) {
        return value;
    }
    codecError("Value " + std::to_string(value) + " outside of range [" + std::to_string(minInclusive) + ":" +
               std::to_string(maxInclusive) + "]");
}

// Normalization.CODEC: Codec.either(Codec.BOOL, Codec.STRING "legacy"); a
// JSON boolean or the string "legacy", nothing else (not a number).
NormalNoise::Normalization readNormalization(const nlohmann::json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? NormalNoise::Normalization::ENABLED : NormalNoise::Normalization::DISABLED;
    }
    if (value.is_string()) {
        const std::string string = value.get<std::string>();
        if (string == "legacy") {
            return NormalNoise::Normalization::LEGACY;
        }
        codecError("Invalid normalization type: " + string);
    }
    codecError("Not a boolean or string: " + value.dump() + " (normalize)");
}

} // namespace

NormalNoisePtr NormalNoise::fromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        codecError("Not a JSON object: " + json.dump());
    }

    Parameters parameters{1.0, 0, 1, Normalization::ENABLED, {}};

    if (const nlohmann::json* value = field(json, "base_amplitude")) {
        parameters.baseAmplitude =
            checkDoubleRange(readDouble(*value, "base_amplitude"), 9.999999747378752E-6, MAX_AMPLITUDE);
    }

    const nlohmann::json* baseOctave = field(json, "base_octave");
    if (baseOctave == nullptr) {
        codecError("No key base_octave in MapLike" + json.dump());
    }
    parameters.baseOctave = checkIntRange(readInt(*baseOctave, "base_octave"), -32, 32);

    if (const nlohmann::json* value = field(json, "octave_count")) {
        parameters.octaveCount = checkIntRange(readInt(*value, "octave_count"), 1, MAX_OCTAVE_COUNT);
    }

    if (const nlohmann::json* value = field(json, "normalize")) {
        parameters.normalize = readNormalization(*value);
    }

    if (const nlohmann::json* value = field(json, "amplitude_modifiers")) {
        if (!value->is_array()) {
            codecError("Not a list: " + value->dump() + " (amplitude_modifiers)");
        }
        if (value->size() > static_cast<size_t>(MAX_OCTAVE_COUNT)) {
            codecError("List too long: " + std::to_string(value->size()) + " > " + std::to_string(MAX_OCTAVE_COUNT));
        }
        for (const nlohmann::json& element : *value) {
            parameters.amplitudeModifiers.push_back(
                checkDoubleRange(readDouble(element, "amplitude_modifiers"), 0.0, MAX_AMPLITUDE));
        }
    }

    // Parameters.validate.
    if (!parameters.amplitudeModifiers.empty() &&
        parameters.amplitudeModifiers.size() != static_cast<size_t>(parameters.octaveCount)) {
        codecError("amplitude_modifiers had size " + std::to_string(parameters.amplitudeModifiers.size()) +
                   ", but octave_count was " + std::to_string(parameters.octaveCount));
    }

    return NormalNoisePtr(new NormalNoise(std::move(parameters)));
}

} // namespace synth

// ---- Holder<NormalNoise> ---------------------------------------------------------
// Holder.Reference compares by identity (the registry key); Holder.Direct by
// value (NormalNoise.equals = parameter equality); a reference never equals a
// direct holder.

bool NoiseHolder::operator==(const NoiseHolder& other) const {
    if (isReference() || other.isReference()) {
        return isReference() && other.isReference() && key == other.key;
    }
    if (value == other.value) {
        return true;
    }
    return value && other.value && *value == *other.value;
}

size_t NoiseHolder::hash() const {
    if (isReference()) {
        return std::hash<std::string>()(key);
    }
    return value ? value->hashCode() : 0;
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
