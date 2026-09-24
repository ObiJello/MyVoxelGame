#pragma once

#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensityFunctionCompiler.h"
#include "levelgen/density/DensityVolume.h"
#include "levelgen/density/SamplerContext.h"
#include "levelgen/density/terrain/Aquifer.h"

#include <memory>

// Reference: levelgen.NoiseChunk (26.3) - one chunk's (or one column's) worth
// of terrain sampling: the volume, a caching SamplerContext over a buffer
// pool borrowed from the RandomState, the samplers bound to it, and the
// aquifer. The structure beardifier rides in as a context field. Returns its
// pool to the RandomState when destroyed (Java's close()).
//
// Blending with pre-1.18 chunks is not implemented (the engine never has old
// chunks next to new ones), so the blender fields are never set and the
// blend_alpha/blend_offset functions read their defaults.

namespace minecraft {
namespace levelgen {
namespace density {

class RandomState;
class TerrainSettings;

class NoiseChunk {
public:
    // beardifier: Beardifier.forStructuresInChunk's result, null = EMPTY
    // (the context field is absent and the function reads its default 0).
    NoiseChunk(RandomState& randomState, std::shared_ptr<const DensitySampler> beardifier,
               const TerrainSettings& settings, Aquifer::FluidPicker globalFluidPicker, const DensityVolume& volume);
    ~NoiseChunk();

    NoiseChunk(const NoiseChunk&) = delete;
    NoiseChunk& operator=(const NoiseChunk&) = delete;

    const DensityVolume& volume() const { return m_volume; }
    Aquifer& aquifer() { return *m_aquifer; }
    const DensitySamplerSet& cachingSamplers() const { return *m_cachingSamplers; }

private:
    RandomState& m_randomState;
    DensityVolume m_volume;
    std::shared_ptr<const DensitySampler> m_beardifier;
    std::unique_ptr<DensityBufferPool> m_bufferPool;
    std::unique_ptr<SamplerContext> m_context;
    std::unique_ptr<DensitySamplerSet> m_cachingSamplers;
    std::unique_ptr<Aquifer> m_aquifer;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
