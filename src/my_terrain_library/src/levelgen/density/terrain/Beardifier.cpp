#include "levelgen/density/terrain/Beardifier.h"
#include "levelgen/density/JavaMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// Reference: levelgen.Beardifier (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

constexpr int BEARD_KERNEL_RADIUS = 12;
constexpr int BEARD_KERNEL_SIZE = 24;

// Mth.lengthSquared(float, float, float).
float lengthSquared(float x, float y, float z) { return x * x + y * y + z * z; }
double lengthSquared(double x, double y, double z) { return x * x + y * y + z * z; }

// Mth.fastInvSqrt(double).
double fastInvSqrt(double x) {
    const double xhalf = 0.5 * x;
    int64_t i;
    std::memcpy(&i, &x, sizeof(i));
    i = 6910469410427058090LL - (i >> 1);
    std::memcpy(&x, &i, sizeof(x));
    x *= 1.5 - xhalf * x * x;
    return x;
}

double computeBeardContribution(int dx, double dy, int dz) {
    const double distanceSqr = lengthSquared(static_cast<double>(dx), dy, static_cast<double>(dz));
    // Math.E (M_E is not standard: MSVC hides it).
    return std::pow(2.718281828459045, -distanceSqr / 16.0);
}

double computeBeardContribution(int dx, int dy, int dz) {
    return computeBeardContribution(dx, static_cast<double>(dy) + 0.5, dz);
}

const std::array<float, 13824>& beardKernel() {
    static const std::array<float, 13824> kernel = [] {
        std::array<float, 13824> k{};
        for (int zi = 0; zi < BEARD_KERNEL_SIZE; ++zi) {
            for (int xi = 0; xi < BEARD_KERNEL_SIZE; ++xi) {
                for (int yi = 0; yi < BEARD_KERNEL_SIZE; ++yi) {
                    k[static_cast<size_t>(zi * 24 * 24 + xi * 24 + yi)] = static_cast<float>(
                        computeBeardContribution(xi - BEARD_KERNEL_RADIUS, yi - BEARD_KERNEL_RADIUS,
                                                 zi - BEARD_KERNEL_RADIUS));
                }
            }
        }
        return k;
    }();
    return kernel;
}

bool isInKernelRange(int xi) { return xi >= 0 && xi < BEARD_KERNEL_SIZE; }

} // namespace

void Beardifier::sampleVolume(SamplerContext&, DensityBuffer& outputBuffer, const DensityVolume& volume) const {
    outputBuffer.fill(0.0f);
    if (!m_affectedBox || !volume.intersects(*m_affectedBox)) return;
    const levelgen::BoundingBox& box = *m_affectedBox;
    const int minX = jmath::floorDiv(std::max(0, box.minX - volume.minBlockX), volume.stepBlockX);
    const int minY = jmath::floorDiv(std::max(0, box.minY - volume.minBlockY), volume.stepBlockY);
    const int minZ = jmath::floorDiv(std::max(0, box.minZ - volume.minBlockZ), volume.stepBlockZ);
    const int maxX = std::min(volume.sizeX - 1, jmath::floorDiv(box.maxX - volume.minBlockX, volume.stepBlockX));
    const int maxY = std::min(volume.sizeY - 1, jmath::floorDiv(box.maxY - volume.minBlockY, volume.stepBlockY));
    const int maxZ = std::min(volume.sizeZ - 1, jmath::floorDiv(box.maxZ - volume.minBlockZ, volume.stepBlockZ));
    for (int z = minZ; z <= maxZ; ++z) {
        const int blockZ = volume.blockZ(z);
        for (int x = minX; x <= maxX; ++x) {
            const int blockX = volume.blockX(x);
            for (int y = minY; y <= maxY; ++y) {
                const int index = volume.indexUnchecked(x, y, z);
                const int blockY = volume.blockY(y);
                outputBuffer.set(index, sampleValueUnchecked(blockX, blockY, blockZ));
            }
        }
    }
}

float Beardifier::sampleValue(SamplerContext&, int blockX, int blockY, int blockZ) const {
    return m_affectedBox && m_affectedBox->isInside(blockX, blockY, blockZ)
        ? sampleValueUnchecked(blockX, blockY, blockZ) : 0.0f;
}

float Beardifier::sampleValueUnchecked(int blockX, int blockY, int blockZ) const {
    float noiseValue = 0.0f;
    for (const levelgen::Rigid& rigid : m_pieces) {
        const levelgen::BoundingBox& box = rigid.box;
        const int groundLevelDelta = rigid.groundLevelDelta;
        const int dx = std::max(0, std::max(box.minX - blockX, blockX - box.maxX));
        const int dz = std::max(0, std::max(box.minZ - blockZ, blockZ - box.maxZ));
        const int groundY = box.minY + groundLevelDelta;
        const int dyToGround = blockY - groundY;
        int dy = 0;
        switch (rigid.terrainAdjustment) {
            case TerrainAdjustment::NONE: dy = 0; break;
            case TerrainAdjustment::BURY:
            case TerrainAdjustment::BEARD_THIN: dy = dyToGround; break;
            case TerrainAdjustment::BEARD_BOX: dy = std::max(0, std::max(groundY - blockY, blockY - box.maxY)); break;
            case TerrainAdjustment::ENCAPSULATE: dy = std::max(0, std::max(box.minY - blockY, blockY - box.maxY)); break;
        }
        float contribution = 0.0f;
        switch (rigid.terrainAdjustment) {
            case TerrainAdjustment::NONE: contribution = 0.0f; break;
            case TerrainAdjustment::BURY:
                contribution = getBuryContribution(static_cast<float>(dx), static_cast<float>(dy) / 2.0f,
                                                   static_cast<float>(dz));
                break;
            case TerrainAdjustment::BEARD_THIN:
            case TerrainAdjustment::BEARD_BOX:
                contribution = getBeardContribution(dx, dy, dz, dyToGround) * 0.8f;
                break;
            case TerrainAdjustment::ENCAPSULATE:
                contribution = getBuryContribution(static_cast<float>(dx) / 2.0f, static_cast<float>(dy) / 2.0f,
                                                   static_cast<float>(dz) / 2.0f) * 0.8f;
                break;
        }
        noiseValue += contribution;
    }
    for (const levelgen::JigsawJunction& junction : m_junctions) {
        const int dx = blockX - junction.sourceX;
        const int dy = blockY - junction.sourceGroundY;
        const int dz = blockZ - junction.sourceZ;
        noiseValue += getBeardContribution(dx, dy, dz, dy) * 0.4f;
    }
    return noiseValue;
}

float Beardifier::getBuryContribution(float dx, float dy, float dz) {
    const float distanceSq = lengthSquared(dx, dy, dz);
    return distanceSq >= 36.0f ? 0.0f
                               : 1.0f - static_cast<float>(std::sqrt(static_cast<double>(distanceSq))) / 6.0f;
}

float Beardifier::getBeardContribution(int dx, int dy, int dz, int yToGround) {
    const int xi = dx + BEARD_KERNEL_RADIUS;
    const int yi = dy + BEARD_KERNEL_RADIUS;
    const int zi = dz + BEARD_KERNEL_RADIUS;
    if (!isInKernelRange(xi) || !isInKernelRange(yi) || !isInKernelRange(zi)) return 0.0f;
    const float dyWithOffset = static_cast<float>(yToGround) + 0.5f;
    const float distanceSqr = lengthSquared(static_cast<float>(dx), dyWithOffset, static_cast<float>(dz));
    const float value = -dyWithOffset * static_cast<float>(fastInvSqrt(static_cast<double>(distanceSqr / 2.0f))) / 2.0f;
    return value * beardKernel()[static_cast<size_t>(zi * 24 * 24 + xi * 24 + yi)];
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
