#include "levelgen/density/synth/SimplexNoise.h"

#include "levelgen/density/JavaMath.h"

#include <cmath>
#include <cstdint>

// Reference: synth.SimplexNoise (26.3).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

namespace {
const double SQRT_3 = std::sqrt(3.0);
const double F2 = 0.5 * (SQRT_3 - 1.0);
const double G2 = (3.0 - SQRT_3) / 6.0;
} // namespace

const Interval SimplexNoise::RANGE = Interval::ofSymmetric(2.0f);

SimplexNoise::SimplexNoise(random::AnyRandomSource& random) : GradientNoise(random) {}

SimplexNoise::SimplexNoise(random::AnyRandomSource& random, bool discardNoiseOffset)
    : GradientNoise(random, discardNoiseOffset ? 0.0 : 256.0) {}

Interval SimplexNoise::range() const {
    return RANGE;
}

double SimplexNoise::getCornerNoise3D(int index, double x, double y, double z, double base) const {
    double t0 = base - x * x - y * y - z * z;
    double n0;
    if (t0 < 0.0) {
        n0 = 0.0;
    } else {
        t0 *= t0;
        n0 = t0 * t0 * GRADIENT[index].dot(x, y, z);
    }
    return n0;
}

float SimplexNoise::get(double _xin, double _yin) const {
    const double xin = _xin + m_offsetX;
    const double yin = _yin + m_offsetY;
    const double s = (xin + yin) * F2;
    const int i = jmath::floor(xin + s);
    const int j = jmath::floor(yin + s);
    const double t = static_cast<double>(i + j) * G2;
    const double X0 = static_cast<double>(i) - t;
    const double Y0 = static_cast<double>(j) - t;
    const double x0 = xin - X0;
    const double y0 = yin - Y0;
    int8_t i1;
    int8_t j1;
    if (x0 > y0) {
        i1 = 1;
        j1 = 0;
    } else {
        i1 = 0;
        j1 = 1;
    }

    const double x1 = x0 - static_cast<double>(i1) + G2;
    const double y1 = y0 - static_cast<double>(j1) + G2;
    const double x2 = x0 - 1.0 + 2.0 * G2;
    const double y2 = y0 - 1.0 + 2.0 * G2;
    const int ii = i & 255;
    const int jj = j & 255;
    const int gi0 = permute(ii + permute(jj)) % 12;
    const int gi1 = permute(ii + i1 + permute(jj + j1)) % 12;
    const int gi2 = permute(ii + 1 + permute(jj + 1)) % 12;
    const double n0 = getCornerNoise3D(gi0, x0, y0, 0.0, 0.5);
    const double n1 = getCornerNoise3D(gi1, x1, y1, 0.0, 0.5);
    const double n2 = getCornerNoise3D(gi2, x2, y2, 0.0, 0.5);
    return static_cast<float>(70.0 * (n0 + n1 + n2));
}

float SimplexNoise::get(double _xin, double _yin, double _zin) const {
    const double xin = _xin + m_offsetX;
    const double yin = _yin + m_offsetY;
    const double zin = _zin + m_offsetZ;
    const double s = (xin + yin + zin) * 0.3333333333333333;
    const int i = jmath::floor(xin + s);
    const int j = jmath::floor(yin + s);
    const int k = jmath::floor(zin + s);
    const double t = static_cast<double>(i + j + k) * 0.16666666666666666;
    const double X0 = static_cast<double>(i) - t;
    const double Y0 = static_cast<double>(j) - t;
    const double Z0 = static_cast<double>(k) - t;
    const double x0 = xin - X0;
    const double y0 = yin - Y0;
    const double z0 = zin - Z0;
    int8_t i1;
    int8_t j1;
    int8_t k1;
    int8_t i2;
    int8_t j2;
    int8_t k2;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
        } else if (x0 >= z0) {
            i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1;
        } else {
            i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1;
        }
    } else if (y0 < z0) {
        i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1;
    } else if (x0 < z0) {
        i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1;
    } else {
        i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0;
    }

    const double x1 = x0 - static_cast<double>(i1) + 0.16666666666666666;
    const double y1 = y0 - static_cast<double>(j1) + 0.16666666666666666;
    const double z1 = z0 - static_cast<double>(k1) + 0.16666666666666666;
    const double x2 = x0 - static_cast<double>(i2) + 0.3333333333333333;
    const double y2 = y0 - static_cast<double>(j2) + 0.3333333333333333;
    const double z2 = z0 - static_cast<double>(k2) + 0.3333333333333333;
    const double x3 = x0 - 1.0 + 0.5;
    const double y3 = y0 - 1.0 + 0.5;
    const double z3 = z0 - 1.0 + 0.5;
    const int ii = i & 255;
    const int jj = j & 255;
    const int kk = k & 255;
    const int gi0 = permute(ii + permute(jj + permute(kk))) % 12;
    const int gi1 = permute(ii + i1 + permute(jj + j1 + permute(kk + k1))) % 12;
    const int gi2 = permute(ii + i2 + permute(jj + j2 + permute(kk + k2))) % 12;
    const int gi3 = permute(ii + 1 + permute(jj + 1 + permute(kk + 1))) % 12;
    const double n0 = getCornerNoise3D(gi0, x0, y0, z0, 0.6);
    const double n1 = getCornerNoise3D(gi1, x1, y1, z1, 0.6);
    const double n2 = getCornerNoise3D(gi2, x2, y2, z2, 0.6);
    const double n3 = getCornerNoise3D(gi3, x3, y3, z3, 0.6);
    return static_cast<float>(32.0 * (n0 + n1 + n2 + n3));
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
