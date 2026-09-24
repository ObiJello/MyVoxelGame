#include "levelgen/density/synth/SmearedPerlinNoise.h"

#include "levelgen/density/JavaMath.h"

#include <cmath>
#include <cstdint>
#include <limits>

// Reference: synth.SmearedPerlinNoise (26.3).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

SmearedPerlinNoise::SmearedPerlinNoise(random::AnyRandomSource& random, double fudgeYScale)
    : PerlinNoise(random), m_fudgeYScale(fudgeYScale) {}

Interval SmearedPerlinNoise::range(double fudgeYScale) {
    return Interval::ofSymmetric(static_cast<float>(std::fabs(fudgeYScale) + 2.0));
}

Interval SmearedPerlinNoise::range() const {
    return range(m_fudgeYScale);
}

float SmearedPerlinNoise::get(double _x, double _y, double _z) const {
    const double x = wrap(_x) + m_offsetX;
    const double y = wrap(_y) + m_offsetY;
    const double z = wrap(_z) + m_offsetZ;
    const int floorX = jmath::floor(x);
    const int floorY = jmath::floor(y);
    const int floorZ = jmath::floor(z);
    const float relativeX = static_cast<float>(x - static_cast<double>(floorX));
    const double relativeY = y - static_cast<double>(floorY);
    const float relativeZ = static_cast<float>(z - static_cast<double>(floorZ));
    const float fudgedRelativeY = static_cast<float>(relativeY - computeFudgeY(_y, relativeY));
    return sampleAndLerp(floorX, floorY, floorZ, relativeX, fudgedRelativeY, relativeZ,
                         static_cast<float>(relativeY));
}

double SmearedPerlinNoise::computeFudgeY(double originalY, double relativeY) const {
    double fudgeLimit;
    if (originalY >= 0.0 && originalY < relativeY) {
        fudgeLimit = originalY;
    } else {
        fudgeLimit = relativeY;
    }
    return static_cast<double>(jmath::floor(fudgeLimit / m_fudgeYScale + 1.0000000116860974E-7)) * m_fudgeYScale;
}

void SmearedPerlinNoise::addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
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
                const double originalY = static_cast<double>(volume.blockY(indexY)) * yScale;
                const double y = wrap(originalY) + m_offsetY;
                const int floorY = jmath::floor(y);
                const double relativeY = y - static_cast<double>(floorY);
                const float alphaY = jmath::smoothstep(static_cast<float>(relativeY));
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

                const float fudgedRelativeY = static_cast<float>(relativeY - computeFudgeY(originalY, relativeY));
                buffer.addTo(index, amplitude * jmath::lerp3(alphaX, alphaY, alphaZ,
                                                             d000xz + g000y * fudgedRelativeY,
                                                             d100xz + g100y * fudgedRelativeY,
                                                             d010xz + g010y * (fudgedRelativeY - 1.0f),
                                                             d110xz + g110y * (fudgedRelativeY - 1.0f),
                                                             d001xz + g001y * fudgedRelativeY,
                                                             d101xz + g101y * fudgedRelativeY,
                                                             d011xz + g011y * (fudgedRelativeY - 1.0f),
                                                             d111xz + g111y * (fudgedRelativeY - 1.0f)));
                ++index;
            }
        }
    }
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
