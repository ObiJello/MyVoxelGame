#include "levelgen/density/synth/SmearedPerlinNoise.h"

#include "levelgen/density/JavaMath.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

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

namespace {

// See PerlinNoise.cpp's VolumeAxes: the walk's per-axis terms once per call.
// Here the Y terms include the smear (computeFudgeY), which depends on the
// block Y alone.
struct SmearedVolumeAxes {
    std::vector<int> floorY;
    std::vector<float> alphaY;
    std::vector<float> fudgedRelativeY;
    std::vector<int> x0;
    std::vector<int> x1;
    std::vector<float> relativeX;
    std::vector<float> alphaX;
};

thread_local SmearedVolumeAxes t_smearedVolumeAxes;

} // namespace

void SmearedPerlinNoise::addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                                     double xzScale, double yScale, float amplitude) const {
    SmearedVolumeAxes& axes = t_smearedVolumeAxes;
    axes.floorY.resize(static_cast<size_t>(volume.sizeY));
    axes.alphaY.resize(static_cast<size_t>(volume.sizeY));
    axes.fudgedRelativeY.resize(static_cast<size_t>(volume.sizeY));
    for (int indexY = 0; indexY < volume.sizeY; ++indexY) {
        const double originalY = static_cast<double>(volume.blockY(indexY)) * yScale;
        const double y = wrap(originalY) + m_offsetY;
        const int floorY = jmath::floor(y);
        const double relativeY = y - static_cast<double>(floorY);
        axes.floorY[indexY] = floorY;
        axes.alphaY[indexY] = jmath::smoothstep(static_cast<float>(relativeY));
        axes.fudgedRelativeY[indexY] = static_cast<float>(relativeY - computeFudgeY(originalY, relativeY));
    }
    axes.x0.resize(static_cast<size_t>(volume.sizeX));
    axes.x1.resize(static_cast<size_t>(volume.sizeX));
    axes.relativeX.resize(static_cast<size_t>(volume.sizeX));
    axes.alphaX.resize(static_cast<size_t>(volume.sizeX));
    for (int indexX = 0; indexX < volume.sizeX; ++indexX) {
        const double x = wrap(static_cast<double>(volume.blockX(indexX)) * xzScale) + m_offsetX;
        const int floorX = jmath::floor(x);
        const float relativeX = static_cast<float>(x - static_cast<double>(floorX));
        axes.x0[indexX] = permute(floorX);
        axes.x1[indexX] = permute(floorX + 1);
        axes.relativeX[indexX] = relativeX;
        axes.alphaX[indexX] = jmath::smoothstep(relativeX);
    }

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
            const float relativeX = axes.relativeX[indexX];
            const int x0 = axes.x0[indexX];
            const int x1 = axes.x1[indexX];
            const float alphaX = axes.alphaX[indexX];
            // Runs of equal floorY share the eight corner gradients: they are
            // refreshed where a run starts - exactly where the per-cell walk's
            // lastFloorY test fired - and the run itself is a branch-free loop
            // over contiguous cells that the compiler vectorizes. Each lane
            // does the scalar walk's operations in the same order (no FMA
            // contraction), so every value is bit-identical.
            float* const out = buffer.data();
            int indexY = 0;
            while (indexY < volume.sizeY) {
                const int floorY = axes.floorY[indexY];
                int runEnd = indexY + 1;
                while (runEnd < volume.sizeY && axes.floorY[runEnd] == floorY) ++runEnd;
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

                float* __restrict runOut = out + index;
                const float* __restrict runRelY = axes.fudgedRelativeY.data() + indexY;
                const float* __restrict runAlphaY = axes.alphaY.data() + indexY;
                const int runLength = runEnd - indexY;
                for (int k = 0; k < runLength; ++k) {
                    const float relY = runRelY[k];
                    runOut[k] += amplitude * jmath::lerp3(alphaX, runAlphaY[k], alphaZ,
                                                          d000xz + g000y * relY,
                                                          d100xz + g100y * relY,
                                                          d010xz + g010y * (relY - 1.0f),
                                                          d110xz + g110y * (relY - 1.0f),
                                                          d001xz + g001y * relY,
                                                          d101xz + g101y * relY,
                                                          d011xz + g011y * (relY - 1.0f),
                                                          d111xz + g111y * (relY - 1.0f));
                }
                index += runLength;
                indexY = runEnd;
            }
        }
    }
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
