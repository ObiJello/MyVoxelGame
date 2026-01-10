#include "synth/BlendedNoise.h"
#include "math/Mth.h"

namespace minecraft {

BlendedNoise BlendedNoise::createUnseeded(double xzScale,
                                          double yScale,
                                          double xzFactor,
                                          double yFactor,
                                          double smearScaleMultiplier) {
    // Reference: BlendedNoise.java lines 31-33
    // Creates a BlendedNoise with seed 0L for consistent unseeded generation
    XoroshiroRandomSource random(0L);
    return BlendedNoise(random, xzScale, yScale, xzFactor, yFactor, smearScaleMultiplier);
}

BlendedNoise::BlendedNoise(XoroshiroRandomSource& random,
                           double xzScale,
                           double yScale,
                           double xzFactor,
                           double yFactor,
                           double smearScaleMultiplier)
{
    // Reference: BlendedNoise.java lines 50-52
    // Creates three PerlinNoise instances using legacy initialization

    // minLimitNoise: octaves -15 to 0 (16 octaves)
    m_minLimitNoise = new PerlinNoise(PerlinNoise::createLegacyForBlendedNoise(random, -15, 0));

    // maxLimitNoise: octaves -15 to 0 (16 octaves)
    m_maxLimitNoise = new PerlinNoise(PerlinNoise::createLegacyForBlendedNoise(random, -15, 0));

    // mainNoise: octaves -7 to 0 (8 octaves)
    m_mainNoise = new PerlinNoise(PerlinNoise::createLegacyForBlendedNoise(random, -7, 0));

    // Store parameters and compute derived values
    m_xzScale = xzScale;
    m_yScale = yScale;
    m_xzFactor = xzFactor;
    m_yFactor = yFactor;
    m_smearScaleMultiplier = smearScaleMultiplier;

    // Reference: BlendedNoise.java lines 44-46
    m_xzMultiplier = 684.412 * m_xzScale;
    m_yMultiplier = 684.412 * m_yScale;
    m_maxValue = m_minLimitNoise->maxBrokenValue(m_yMultiplier);
}

BlendedNoise::BlendedNoise(PerlinNoise* minLimitNoise,
                           PerlinNoise* maxLimitNoise,
                           PerlinNoise* mainNoise,
                           double xzScale,
                           double yScale,
                           double xzFactor,
                           double yFactor,
                           double smearScaleMultiplier)
    : m_minLimitNoise(minLimitNoise)
    , m_maxLimitNoise(maxLimitNoise)
    , m_mainNoise(mainNoise)
    , m_xzScale(xzScale)
    , m_yScale(yScale)
    , m_xzFactor(xzFactor)
    , m_yFactor(yFactor)
    , m_smearScaleMultiplier(smearScaleMultiplier)
{
    // Reference: BlendedNoise.java lines 35-47
    m_xzMultiplier = 684.412 * m_xzScale;
    m_yMultiplier = 684.412 * m_yScale;
    m_maxValue = m_minLimitNoise->maxBrokenValue(m_yMultiplier);
}

BlendedNoise BlendedNoise::withNewRandom(XoroshiroRandomSource& terrainRandom) const {
    // Reference: BlendedNoise.java lines 54-56
    return BlendedNoise(terrainRandom, m_xzScale, m_yScale, m_xzFactor, m_yFactor, m_smearScaleMultiplier);
}

double BlendedNoise::compute(const FunctionContext& context) const {
    // Reference: BlendedNoise.java lines 58-110

    // Calculate scaled coordinates for limit noises
    // Reference: BlendedNoise.java lines 59-61
    double limitX = static_cast<double>(context.blockX()) * m_xzMultiplier;
    double limitY = static_cast<double>(context.blockY()) * m_yMultiplier;
    double limitZ = static_cast<double>(context.blockZ()) * m_xzMultiplier;

    // Calculate scaled coordinates for main noise
    // Reference: BlendedNoise.java lines 62-64
    double mainX = limitX / m_xzFactor;
    double mainY = limitY / m_yFactor;
    double mainZ = limitZ / m_xzFactor;

    // Calculate smear values
    // Reference: BlendedNoise.java lines 65-66
    double limitSmear = m_yMultiplier * m_smearScaleMultiplier;
    double mainSmear = limitSmear / m_yFactor;

    // Initialize accumulators
    // Reference: BlendedNoise.java lines 67-71
    double blendMin = 0.0;
    double blendMax = 0.0;
    double mainNoiseValue = 0.0;
    double pow = 1.0;

    // First loop: Sample main noise (8 octaves)
    // Reference: BlendedNoise.java lines 73-80
    for (int32_t i = 0; i < 8; ++i) {
        ImprovedNoise* noise = m_mainNoise->getOctaveNoise(i);
        if (noise != nullptr) {
            mainNoiseValue += noise->noise(
                PerlinNoise::wrap(mainX * pow),
                PerlinNoise::wrap(mainY * pow),
                PerlinNoise::wrap(mainZ * pow),
                mainSmear * pow,
                mainY * pow
            ) / pow;
        }

        pow /= 2.0;
    }

    // Calculate blending factor
    // Reference: BlendedNoise.java lines 82-84
    double factor = (mainNoiseValue / 10.0 + 1.0) / 2.0;
    bool isMax = factor >= 1.0;
    bool isMin = factor <= 0.0;

    // Reset pow for second loop
    // Reference: BlendedNoise.java line 85
    pow = 1.0;

    // Second loop: Sample min/max limit noises (16 octaves)
    // Reference: BlendedNoise.java lines 87-107
    for (int32_t i = 0; i < 16; ++i) {
        double wx = PerlinNoise::wrap(limitX * pow);
        double wy = PerlinNoise::wrap(limitY * pow);
        double wz = PerlinNoise::wrap(limitZ * pow);
        double yScalePow = limitSmear * pow;

        // Sample minLimitNoise if not at max
        if (!isMax) {
            ImprovedNoise* minNoise = m_minLimitNoise->getOctaveNoise(i);
            if (minNoise != nullptr) {
                blendMin += minNoise->noise(wx, wy, wz, yScalePow, limitY * pow) / pow;
            }
        }

        // Sample maxLimitNoise if not at min
        if (!isMin) {
            ImprovedNoise* maxNoise = m_maxLimitNoise->getOctaveNoise(i);
            if (maxNoise != nullptr) {
                blendMax += maxNoise->noise(wx, wy, wz, yScalePow, limitY * pow) / pow;
            }
        }

        pow /= 2.0;
    }

    // Final blend and normalization
    // Reference: BlendedNoise.java line 109
    return Mth::clampedLerp(factor, blendMin / 512.0, blendMax / 512.0) / 128.0;
}

double BlendedNoise::minValue() const {
    // Reference: BlendedNoise.java lines 112-114
    return -maxValue();
}

double BlendedNoise::maxValue() const {
    // Reference: BlendedNoise.java lines 116-118
    return m_maxValue;
}

void BlendedNoise::fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const {
    // Default implementation using compute()
    // Note: const_cast needed because fillAllDirectly expects non-const pointer
    contextProvider.fillAllDirectly(output, count, const_cast<BlendedNoise*>(this));
}

DensityFunction* BlendedNoise::mapAll(Visitor& visitor) {
    // BlendedNoise doesn't transform its internal structure
    return visitor.apply(this);
}

} // namespace minecraft
