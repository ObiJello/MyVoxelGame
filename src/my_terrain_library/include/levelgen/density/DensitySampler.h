#pragma once

#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensityVolume.h"

#include <memory>

// Reference: densityfunction.DensitySampler (26.3) - a compiled density
// function. sampleValue answers one block; sampleVolume fills a buffer for a
// strided box. The two paths are NOT interchangeable bit for bit (a volume may
// sum in another order), and callers pick the one Java picks. Samplers are
// immutable once compiled and shared between threads; all mutable state lives
// in the SamplerContext.

namespace minecraft {
namespace levelgen {
namespace density {

class SamplerContext;

class DensitySampler {
public:
    virtual ~DensitySampler() = default;

    virtual void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                              const DensityVolume& volume) const = 0;
    virtual float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const = 0;

    // DensitySampler.sampleVolumeNaive: one sampleValue per volume cell.
    static void sampleVolumeNaive(SamplerContext& context, DensityBuffer& outputBuffer,
                                  const DensityVolume& volume, const DensitySampler& sampler) {
        int index = 0;
        for (int z = 0; z < volume.sizeZ; ++z) {
            const int blockZ = volume.blockZ(z);
            for (int x = 0; x < volume.sizeX; ++x) {
                const int blockX = volume.blockX(x);
                for (int y = 0; y < volume.sizeY; ++y) {
                    const int blockY = volume.blockY(y);
                    outputBuffer.set(index++, sampler.sampleValue(context, blockX, blockY, blockZ));
                }
            }
        }
    }
};

using DensitySamplerPtr = std::shared_ptr<const DensitySampler>;

// DensitySampler.Bound: a sampler with the context it samples in.
struct BoundSampler {
    const DensitySampler* sampler = nullptr;
    SamplerContext* context = nullptr;

    float sampleValue(int blockX, int blockY, int blockZ) const {
        return sampler->sampleValue(*context, blockX, blockY, blockZ);
    }
    void sampleVolume(DensityBuffer& outputBuffer, const DensityVolume& volume) const {
        sampler->sampleVolume(*context, outputBuffer, volume);
    }
    ScopedBuffer sampleVolume(const DensityVolume& volume) const;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
