#pragma once

#include "levelgen/density/JavaMath.h"

#include <stdexcept>

// Reference: net.minecraft.world.level.levelgen.densityfunction.DensityVolume
// (26.3) - a strided box of block positions a sampler fills in one call.
// Buffer order is y fastest, then x, then z: indexUnchecked(x, y, z) =
// y + (x + z * sizeX) * sizeY.

namespace minecraft {
namespace levelgen {
namespace density {

struct DensityVolume {
    static constexpr int NO_BLOCK = -1;

    int sizeX = 1, sizeY = 1, sizeZ = 1;
    int minBlockX = 0, minBlockY = 0, minBlockZ = 0;
    int stepBlockX = 1, stepBlockY = 1, stepBlockZ = 1;

    DensityVolume() = default;

    DensityVolume(int sizeX, int sizeY, int sizeZ, int minBlockX, int minBlockY, int minBlockZ,
                  int stepBlockX = 1, int stepBlockY = 1, int stepBlockZ = 1)
        : sizeX(sizeX), sizeY(sizeY), sizeZ(sizeZ),
          minBlockX(minBlockX), minBlockY(minBlockY), minBlockZ(minBlockZ),
          stepBlockX(stepBlockX), stepBlockY(stepBlockY), stepBlockZ(stepBlockZ) {
        if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0) {
            throw std::invalid_argument("DensityVolume size must be positive");
        }
        if (stepBlockX <= 0 || stepBlockY <= 0 || stepBlockZ <= 0) {
            throw std::invalid_argument("DensityVolume step must be positive");
        }
    }

    int indexUnchecked(int indexX, int indexY, int indexZ) const {
        return indexY + (indexX + indexZ * sizeX) * sizeY;
    }
    int blockX(int x) const { return minBlockX + x * stepBlockX; }
    int blockY(int y) const { return minBlockY + y * stepBlockY; }
    int blockZ(int z) const { return minBlockZ + z * stepBlockZ; }
    int maxBlockX() const { return minBlockX + sizeX * stepBlockX - 1; }
    int maxBlockY() const { return minBlockY + sizeY * stepBlockY - 1; }
    int maxBlockZ() const { return minBlockZ + sizeZ * stepBlockZ - 1; }
    int size() const { return sizeX * sizeY * sizeZ; }

    // BoundingBox.intersects(minX, minY, minZ, maxX, maxY, maxZ).
    template <typename Box>
    bool intersects(const Box& box) const {
        return box.maxX >= minBlockX && box.minX <= maxBlockX() &&
               box.maxY >= minBlockY && box.minY <= maxBlockY() &&
               box.maxZ >= minBlockZ && box.minZ <= maxBlockZ();
    }

    int indexOfBlock(int blockX, int blockY, int blockZ) const {
        const int relativeX = blockX - minBlockX;
        const int relativeY = blockY - minBlockY;
        const int relativeZ = blockZ - minBlockZ;
        if (stepBlockX == 1 && stepBlockY == 1 && stepBlockZ == 1) {
            if (relativeX >= 0 && relativeY >= 0 && relativeZ >= 0 &&
                relativeX < sizeX && relativeY < sizeY && relativeZ < sizeZ) {
                return indexUnchecked(relativeX, relativeY, relativeZ);
            }
        } else if (containsBlockRelative(relativeX, relativeY, relativeZ)) {
            return indexUnchecked(jmath::floorDiv(relativeX, stepBlockX),
                                  jmath::floorDiv(relativeY, stepBlockY),
                                  jmath::floorDiv(relativeZ, stepBlockZ));
        }
        return NO_BLOCK;
    }

    bool operator==(const DensityVolume& o) const {
        return sizeX == o.sizeX && sizeY == o.sizeY && sizeZ == o.sizeZ &&
               minBlockX == o.minBlockX && minBlockY == o.minBlockY && minBlockZ == o.minBlockZ &&
               stepBlockX == o.stepBlockX && stepBlockY == o.stepBlockY && stepBlockZ == o.stepBlockZ;
    }
    bool operator!=(const DensityVolume& o) const { return !(*this == o); }

private:
    bool containsBlockRelative(int relativeX, int relativeY, int relativeZ) const {
        return relativeX >= 0 && relativeY >= 0 && relativeZ >= 0 &&
               relativeX < sizeX * stepBlockX && relativeY < sizeY * stepBlockY &&
               relativeZ < sizeZ * stepBlockZ &&
               jmath::floorMod(relativeX, stepBlockX) == 0 &&
               jmath::floorMod(relativeY, stepBlockY) == 0 &&
               jmath::floorMod(relativeZ, stepBlockZ) == 0;
    }
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
