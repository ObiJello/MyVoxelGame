#include "levelgen/density/synth/GradientNoise.h"

#include <cmath>

// Reference: synth.GradientNoise (26.3).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

const GradientNoise::Gradient GradientNoise::GRADIENT[16] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
    {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
    {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1},
};

namespace {
// Math.nextDown(1.6777216E7) = 2^24 - 2^-29.
constexpr double HALF_ROUND_OFF = 0x1.fffffffffffffp+23;
static_assert(HALF_ROUND_OFF < 1.6777216E7 && HALF_ROUND_OFF + 0x1p-29 == 1.6777216E7, "nextDown(2^24)");
} // namespace

GradientNoise::GradientNoise(random::AnyRandomSource& random) : GradientNoise(random, 256.0) {}

GradientNoise::GradientNoise(random::AnyRandomSource& random, double noiseOffsetScale) {
    m_offsetX = random.nextDouble() * noiseOffsetScale;
    m_offsetY = random.nextDouble() * noiseOffsetScale;
    m_offsetZ = random.nextDouble() * noiseOffsetScale;

    for (int i = 0; i < 256; ++i) {
        m_perms[i] = static_cast<int8_t>(i);
    }
    for (int i = 0; i < 256; ++i) {
        const int offset = random.nextInt(256 - i);
        const int8_t tmp = m_perms[i];
        m_perms[i] = m_perms[offset + i];
        m_perms[offset + i] = tmp;
    }
}

// SharedConstants.DEBUG_ENABLE_FARLANDS is false.
double GradientNoise::wrap(double x) {
    return x >= -HALF_ROUND_OFF && x < HALF_ROUND_OFF
               ? x
               : x - std::floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
