#include "levelgen/density/synth/BlendedNoise.h"

#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/generator/NoiseFunction.h"
#include "levelgen/density/op/BinaryFunction.h"
#include "levelgen/density/op/ClampFunction.h"
#include "levelgen/density/op/LerpFunction.h"
#include "levelgen/density/synth/SmearedPerlinNoise.h"

#include <cmath>
#include <memory>
#include <stdexcept>

// Reference: synth.BlendedNoise (26.3).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

namespace {
constexpr double LIMIT_FACTOR = 0.9999847412109375;
constexpr double MAIN_FACTOR = 12.75;
constexpr int LIMIT_FIRST_OCTAVE = -15;
constexpr int MAIN_FIRST_OCTAVE = -7;
} // namespace

BlendedNoise::FbmSet BlendedNoise::createFbmSet(random::AnyRandomSource& random) const {
    const double limitSmearScaleY = yMultiplier() * m_smearScaleMultiplier;
    const double mainSmearScaleY = limitSmearScaleY / m_yFactor;
    // Java evaluates the constructor arguments left to right, all from the
    // same random: keep them sequenced.
    NoiseStackPtr minLimitNoise = createFbm(random, LIMIT_FIRST_OCTAVE, limitSmearScaleY, LIMIT_FACTOR);
    NoiseStackPtr maxLimitNoise = createFbm(random, LIMIT_FIRST_OCTAVE, limitSmearScaleY, LIMIT_FACTOR);
    NoiseStackPtr mainNoise = createFbm(random, MAIN_FIRST_OCTAVE, mainSmearScaleY, MAIN_FACTOR);
    return FbmSet{std::move(minLimitNoise), std::move(maxLimitNoise), std::move(mainNoise)};
}

NoiseStackPtr BlendedNoise::createFbm(random::AnyRandomSource& random, int firstOctave, double smearScaleY,
                                      double valueFactor) {
    if (firstOctave > 0) {
        throw std::invalid_argument("firstOctave>0");
    }
    const int octaves = -firstOctave + 1;
    double factor = 1.0;
    valueFactor /= std::pow(2.0, static_cast<double>(octaves)) - 1.0;
    NoiseStack::Builder stack = NoiseStack::builder();

    for (int i = octaves - 1; i >= 0; --i) {
        stack.add(std::make_shared<const SmearedPerlinNoise>(random, smearScaleY * factor), factor,
                  static_cast<float>(valueFactor));
        factor /= 2.0;
        valueFactor *= 2.0;
    }
    return stack.build();
}

Interval BlendedNoise::computeFbmRange(int firstOctave, double smearScaleY, double valueFactor) {
    const int octaves = -firstOctave + 1;
    double factor = 1.0;
    valueFactor /= std::pow(2.0, static_cast<double>(octaves)) - 1.0;
    Interval range = Interval::ofExact(0.0f);

    for (int i = octaves - 1; i >= 0; --i) {
        const Interval layerRange = Interval::mul(SmearedPerlinNoise::range(smearScaleY * factor),
                                                  Interval::ofExact(static_cast<float>(valueFactor)));
        range = Interval::add(range, layerRange);
        factor /= 2.0;
        valueFactor *= 2.0;
    }
    return range;
}

DensitySamplerPtr BlendedNoise::compileSampler(CompileContext& context) const {
    random::AnyRandomSource random = context.createRandom(NOISE_SEED);
    return compileSampler(random);
}

DensitySamplerPtr BlendedNoise::compileSampler(random::AnyRandomSource& random) const {
    const FbmSet fbms = createFbmSet(random);
    const double xzMultiplier = this->xzMultiplier();
    const double yMultiplier = this->yMultiplier();
    DensitySamplerPtr minLimitNoise = std::make_shared<NoiseFunction::Sampler>(fbms.minLimitNoise, xzMultiplier, yMultiplier);
    DensitySamplerPtr maxLimitNoise = std::make_shared<NoiseFunction::Sampler>(fbms.maxLimitNoise, xzMultiplier, yMultiplier);
    DensitySamplerPtr mainNoise = std::make_shared<NoiseFunction::Sampler>(
        fbms.mainNoise, xzMultiplier / m_xzFactor, yMultiplier / m_yFactor);
    DensitySamplerPtr choice = std::make_shared<ClampFunction::Sampler>(
        std::make_shared<BinaryFunction::ConstAddSampler>(mainNoise, 0.5f), 0.0f, 1.0f);
    return std::make_shared<LerpFunction::Sampler>(choice, minLimitNoise, maxLimitNoise);
}

DensityFunctionPtr BlendedNoise::rewriteChildren(const DfRewriteRule&) const {
    return self();
}

Interval BlendedNoise::range() const {
    return computeFbmRange(LIMIT_FIRST_OCTAVE, yMultiplier() * m_smearScaleMultiplier, LIMIT_FACTOR);
}

bool BlendedNoise::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const BlendedNoise*>(&other);
    return o != nullptr && doubleEq(m_xzScale, o->m_xzScale) && doubleEq(m_yScale, o->m_yScale) &&
           doubleEq(m_xzFactor, o->m_xzFactor) && doubleEq(m_yFactor, o->m_yFactor) &&
           doubleEq(m_smearScaleMultiplier, o->m_smearScaleMultiplier);
}

size_t BlendedNoise::hash() const {
    size_t h = hashDouble(m_xzScale);
    h = hashCombine(h, hashDouble(m_yScale));
    h = hashCombine(h, hashDouble(m_xzFactor));
    h = hashCombine(h, hashDouble(m_yFactor));
    h = hashCombine(h, hashDouble(m_smearScaleMultiplier));
    return h;
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
