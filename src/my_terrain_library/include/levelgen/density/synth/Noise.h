#pragma once

#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensityVolume.h"
#include "levelgen/density/Interval.h"

#include <memory>

// Reference: levelgen.synth.Noise (26.3) - a float noise over double
// coordinates. addToVolume adds amplitude * noise at every volume cell (x,
// y and z scaled); implementations override it with the cheaper, differently
// rounded volume walk MC uses (PerlinNoise.addToVolume) — keep both paths.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class Noise {
public:
    virtual ~Noise() = default;

    virtual Interval range() const = 0;
    virtual float get(double x, double y) const = 0;
    virtual float get(double x, double y, double z) const = 0;

    virtual void addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                             double xzScale, double yScale, float amplitude) const {
        int index = 0;
        for (int indexZ = 0; indexZ < volume.sizeZ; ++indexZ) {
            const double z = static_cast<double>(volume.blockZ(indexZ)) * xzScale;
            for (int indexX = 0; indexX < volume.sizeX; ++indexX) {
                const double x = static_cast<double>(volume.blockX(indexX)) * xzScale;
                for (int indexY = 0; indexY < volume.sizeY; ++indexY) {
                    const double y = static_cast<double>(volume.blockY(indexY)) * yScale;
                    buffer.addTo(index, amplitude * get(x, y, z));
                    ++index;
                }
            }
        }
    }
};

using NoisePtr = std::shared_ptr<const Noise>;

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
