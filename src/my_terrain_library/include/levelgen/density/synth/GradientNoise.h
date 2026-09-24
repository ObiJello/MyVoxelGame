#pragma once

#include "levelgen/density/synth/Noise.h"
#include "random/AnyPositionalRandomFactory.h"

#include <cstdint>

// Reference: synth.GradientNoise (26.3) - the permutation table, the random
// offsets and the 16 gradients shared by PerlinNoise and SimplexNoise.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class GradientNoise : public Noise {
public:
    // GradientNoise.Gradient (record).
    struct Gradient {
        int xv;
        int yv;
        int zv;

        double dot(double x, double y, double z) const {
            return static_cast<double>(xv) * x + static_cast<double>(yv) * y + static_cast<double>(zv) * z;
        }
        float dot(float x, float y, float z) const {
            return static_cast<float>(xv) * x + static_cast<float>(yv) * y + static_cast<float>(zv) * z;
        }
        float dotXz(float x, float z) const {
            return static_cast<float>(xv) * x + static_cast<float>(zv) * z;
        }
        int x() const { return xv; }
        int y() const { return yv; }
        int z() const { return zv; }
    };

    static const Gradient GRADIENT[16];

protected:
    explicit GradientNoise(random::AnyRandomSource& random);
    GradientNoise(random::AnyRandomSource& random, double noiseOffsetScale);

    int permute(int x) const { return static_cast<int>(m_perms[x & 255]) & 255; }
    const Gradient& permuteToGrad(int x) const { return GRADIENT[permute(x) & 15]; }

    static float gradDot(int hash, float x, float y, float z) { return GRADIENT[hash & 15].dot(x, y, z); }
    static double wrap(double x);

    int8_t m_perms[256];
    double m_offsetX;
    double m_offsetY;
    double m_offsetZ;
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
