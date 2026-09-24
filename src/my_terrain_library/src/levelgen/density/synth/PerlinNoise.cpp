#include "levelgen/density/synth/PerlinNoise.h"

#include "levelgen/density/JavaMath.h"

#include <cstdint>
#include <limits>

// Reference: synth.PerlinNoise (26.3).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

const Interval PerlinNoise::RANGE = Interval::ofSymmetric(2.0f);

PerlinNoise::PerlinNoise(random::AnyRandomSource& random) : GradientNoise(random) {}

Interval PerlinNoise::range() const {
    return RANGE;
}

float PerlinNoise::get(double x, double y) const {
    return get(wrap(x), 0.0, wrap(y));
}

float PerlinNoise::get(double _x, double _y, double _z) const {
    const double x = wrap(_x) + m_offsetX;
    const double y = wrap(_y) + m_offsetY;
    const double z = wrap(_z) + m_offsetZ;
    const int floorX = jmath::floor(x);
    const int floorY = jmath::floor(y);
    const int floorZ = jmath::floor(z);
    const float relativeX = static_cast<float>(x - static_cast<double>(floorX));
    const float relativeY = static_cast<float>(y - static_cast<double>(floorY));
    const float relativeZ = static_cast<float>(z - static_cast<double>(floorZ));
    return sampleAndLerp(floorX, floorY, floorZ, relativeX, relativeY, relativeZ, relativeY);
}

float PerlinNoise::noiseWithDerivative(double _x, double _y, double _z, float* derivativeOut) const {
    const double x = wrap(_x) + m_offsetX;
    const double y = wrap(_y) + m_offsetY;
    const double z = wrap(_z) + m_offsetZ;
    const int floorX = jmath::floor(x);
    const int floorY = jmath::floor(y);
    const int floorZ = jmath::floor(z);
    const float relativeX = static_cast<float>(x - static_cast<double>(floorX));
    const float relativeY = static_cast<float>(y - static_cast<double>(floorY));
    const float relativeZ = static_cast<float>(z - static_cast<double>(floorZ));
    return sampleWithDerivative(floorX, floorY, floorZ, relativeX, relativeY, relativeZ, derivativeOut);
}

float PerlinNoise::sampleAndLerp(int x, int y, int z, float relativeX, float relativeY, float relativeZ,
                                 float originalRelativeY) const {
    const int x0 = permute(x);
    const int x1 = permute(x + 1);
    const int xy00 = permute(x0 + y);
    const int xy01 = permute(x0 + y + 1);
    const int xy10 = permute(x1 + y);
    const int xy11 = permute(x1 + y + 1);
    const float d000 = gradDot(permute(xy00 + z), relativeX, relativeY, relativeZ);
    const float d100 = gradDot(permute(xy10 + z), relativeX - 1.0f, relativeY, relativeZ);
    const float d010 = gradDot(permute(xy01 + z), relativeX, relativeY - 1.0f, relativeZ);
    const float d110 = gradDot(permute(xy11 + z), relativeX - 1.0f, relativeY - 1.0f, relativeZ);
    const float d001 = gradDot(permute(xy00 + z + 1), relativeX, relativeY, relativeZ - 1.0f);
    const float d101 = gradDot(permute(xy10 + z + 1), relativeX - 1.0f, relativeY, relativeZ - 1.0f);
    const float d011 = gradDot(permute(xy01 + z + 1), relativeX, relativeY - 1.0f, relativeZ - 1.0f);
    const float d111 = gradDot(permute(xy11 + z + 1), relativeX - 1.0f, relativeY - 1.0f, relativeZ - 1.0f);
    const float xAlpha = jmath::smoothstep(relativeX);
    const float yAlpha = jmath::smoothstep(originalRelativeY);
    const float zAlpha = jmath::smoothstep(relativeZ);
    return jmath::lerp3(xAlpha, yAlpha, zAlpha, d000, d100, d010, d110, d001, d101, d011, d111);
}

void PerlinNoise::addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                              double xzScale, double yScale, float amplitude) const {
    float d000xz = 0.0f;
    float d100xz = 0.0f;
    float d010xz = 0.0f;
    float d110xz = 0.0f;
    float d001xz = 0.0f;
    float d101xz = 0.0f;
    float d011xz = 0.0f;
    float d111xz = 0.0f;
    float g000y = 0.0f;
    float g100y = 0.0f;
    float g010y = 0.0f;
    float g110y = 0.0f;
    float g001y = 0.0f;
    float g101y = 0.0f;
    float g011y = 0.0f;
    float g111y = 0.0f;
    int index = 0;

    for (int indexZ = 0; indexZ < volume.sizeZ; ++indexZ) {
        const double z = wrap(static_cast<double>(volume.blockZ(indexZ)) * xzScale) + m_offsetZ;
        const int floorZ = jmath::floor(z);
        const float relativeZ = static_cast<float>(z - static_cast<double>(floorZ));
        const float alphaZ = jmath::smoothstep(relativeZ);

        for (int indexX = 0; indexX < volume.sizeX; ++indexX) {
            const double x = wrap(static_cast<double>(volume.blockX(indexX)) * xzScale) + m_offsetX;
            const int floorX = jmath::floor(x);
            const float relativeX = static_cast<float>(x - static_cast<double>(floorX));
            const int x0 = permute(floorX);
            const int x1 = permute(floorX + 1);
            const float alphaX = jmath::smoothstep(relativeX);
            int lastFloorY = std::numeric_limits<int32_t>::min();

            for (int indexY = 0; indexY < volume.sizeY; ++indexY) {
                const double y = wrap(static_cast<double>(volume.blockY(indexY)) * yScale) + m_offsetY;
                const int floorY = jmath::floor(y);
                const float relativeY = static_cast<float>(y - static_cast<double>(floorY));
                const float alphaY = jmath::smoothstep(relativeY);
                if (lastFloorY != floorY) {
                    const int xy00 = permute(x0 + floorY);
                    const int xy01 = permute(x0 + floorY + 1);
                    const int xy10 = permute(x1 + floorY);
                    const int xy11 = permute(x1 + floorY + 1);
                    const Gradient& g000 = permuteToGrad(xy00 + floorZ);
                    d000xz = g000.dotXz(relativeX, relativeZ);
                    g000y = static_cast<float>(g000.y());
                    const Gradient& g100 = permuteToGrad(xy10 + floorZ);
                    d100xz = g100.dotXz(relativeX - 1.0f, relativeZ);
                    g100y = static_cast<float>(g100.y());
                    const Gradient& g010 = permuteToGrad(xy01 + floorZ);
                    d010xz = g010.dotXz(relativeX, relativeZ);
                    g010y = static_cast<float>(g010.y());
                    const Gradient& g110 = permuteToGrad(xy11 + floorZ);
                    d110xz = g110.dotXz(relativeX - 1.0f, relativeZ);
                    g110y = static_cast<float>(g110.y());
                    const Gradient& g001 = permuteToGrad(xy00 + floorZ + 1);
                    d001xz = g001.dotXz(relativeX, relativeZ - 1.0f);
                    g001y = static_cast<float>(g001.y());
                    const Gradient& g101 = permuteToGrad(xy10 + floorZ + 1);
                    d101xz = g101.dotXz(relativeX - 1.0f, relativeZ - 1.0f);
                    g101y = static_cast<float>(g101.y());
                    const Gradient& g011 = permuteToGrad(xy01 + floorZ + 1);
                    d011xz = g011.dotXz(relativeX, relativeZ - 1.0f);
                    g011y = static_cast<float>(g011.y());
                    const Gradient& g111 = permuteToGrad(xy11 + floorZ + 1);
                    d111xz = g111.dotXz(relativeX - 1.0f, relativeZ - 1.0f);
                    g111y = static_cast<float>(g111.y());
                    lastFloorY = floorY;
                }

                buffer.addTo(index, amplitude * jmath::lerp3(alphaX, alphaY, alphaZ,
                                                             d000xz + g000y * relativeY,
                                                             d100xz + g100y * relativeY,
                                                             d010xz + g010y * (relativeY - 1.0f),
                                                             d110xz + g110y * (relativeY - 1.0f),
                                                             d001xz + g001y * relativeY,
                                                             d101xz + g101y * relativeY,
                                                             d011xz + g011y * (relativeY - 1.0f),
                                                             d111xz + g111y * (relativeY - 1.0f)));
                ++index;
            }
        }
    }
}

float PerlinNoise::sampleWithDerivative(int x, int y, int z, float xr, float yr, float zr,
                                        float* derivativeOut) const {
    const int x0 = permute(x);
    const int x1 = permute(x + 1);
    const int xy00 = permute(x0 + y);
    const int xy01 = permute(x0 + y + 1);
    const int xy10 = permute(x1 + y);
    const int xy11 = permute(x1 + y + 1);
    const Gradient& g000 = permuteToGrad(xy00 + z);
    const Gradient& g100 = permuteToGrad(xy10 + z);
    const Gradient& g010 = permuteToGrad(xy01 + z);
    const Gradient& g110 = permuteToGrad(xy11 + z);
    const Gradient& g001 = permuteToGrad(xy00 + z + 1);
    const Gradient& g101 = permuteToGrad(xy10 + z + 1);
    const Gradient& g011 = permuteToGrad(xy01 + z + 1);
    const Gradient& g111 = permuteToGrad(xy11 + z + 1);
    const float d000 = g000.dot(xr, yr, zr);
    const float d100 = g100.dot(xr - 1.0f, yr, zr);
    const float d010 = g010.dot(xr, yr - 1.0f, zr);
    const float d110 = g110.dot(xr - 1.0f, yr - 1.0f, zr);
    const float d001 = g001.dot(xr, yr, zr - 1.0f);
    const float d101 = g101.dot(xr - 1.0f, yr, zr - 1.0f);
    const float d011 = g011.dot(xr, yr - 1.0f, zr - 1.0f);
    const float d111 = g111.dot(xr - 1.0f, yr - 1.0f, zr - 1.0f);
    const float xAlpha = jmath::smoothstep(xr);
    const float yAlpha = jmath::smoothstep(yr);
    const float zAlpha = jmath::smoothstep(zr);
    const float d1x = jmath::lerp3(xAlpha, yAlpha, zAlpha,
                                   static_cast<float>(g000.x()), static_cast<float>(g100.x()),
                                   static_cast<float>(g010.x()), static_cast<float>(g110.x()),
                                   static_cast<float>(g001.x()), static_cast<float>(g101.x()),
                                   static_cast<float>(g011.x()), static_cast<float>(g111.x()));
    const float d1y = jmath::lerp3(xAlpha, yAlpha, zAlpha,
                                   static_cast<float>(g000.y()), static_cast<float>(g100.y()),
                                   static_cast<float>(g010.y()), static_cast<float>(g110.y()),
                                   static_cast<float>(g001.y()), static_cast<float>(g101.y()),
                                   static_cast<float>(g011.y()), static_cast<float>(g111.y()));
    const float d1z = jmath::lerp3(xAlpha, yAlpha, zAlpha,
                                   static_cast<float>(g000.z()), static_cast<float>(g100.z()),
                                   static_cast<float>(g010.z()), static_cast<float>(g110.z()),
                                   static_cast<float>(g001.z()), static_cast<float>(g101.z()),
                                   static_cast<float>(g011.z()), static_cast<float>(g111.z()));
    const float d2x = jmath::lerp2(yAlpha, zAlpha, d100 - d000, d110 - d010, d101 - d001, d111 - d011);
    const float d2y = jmath::lerp2(zAlpha, xAlpha, d010 - d000, d011 - d001, d110 - d100, d111 - d101);
    const float d2z = jmath::lerp2(xAlpha, yAlpha, d001 - d000, d101 - d100, d011 - d010, d111 - d110);
    const float xSD = jmath::smoothstepDerivative(xr);
    const float ySD = jmath::smoothstepDerivative(yr);
    const float zSD = jmath::smoothstepDerivative(zr);
    const float dX = d1x + xSD * d2x;
    const float dY = d1y + ySD * d2y;
    const float dZ = d1z + zSD * d2z;
    derivativeOut[0] += dX;
    derivativeOut[1] += dY;
    derivativeOut[2] += dZ;
    return jmath::lerp3(xAlpha, yAlpha, zAlpha, d000, d100, d010, d110, d001, d101, d011, d111);
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
